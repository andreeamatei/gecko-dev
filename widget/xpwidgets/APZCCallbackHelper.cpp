/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "APZCCallbackHelper.h"
#include "gfxPrefs.h" // For gfxPrefs::LayersTilesEnabled
#include "mozilla/Preferences.h"
#include "nsIScrollableFrame.h"
#include "nsLayoutUtils.h"
#include "nsIDOMElement.h"
#include "nsIInterfaceRequestorUtils.h"
#include "TiledLayerBuffer.h" // For TILEDLAYERBUFFER_TILE_SIZE

namespace mozilla {
namespace widget {

bool
APZCCallbackHelper::HasValidPresShellId(nsIDOMWindowUtils* aUtils,
                                        const FrameMetrics& aMetrics)
{
    MOZ_ASSERT(aUtils);

    uint32_t presShellId;
    nsresult rv = aUtils->GetPresShellId(&presShellId);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
    return NS_SUCCEEDED(rv) && aMetrics.mPresShellId == presShellId;
}

/**
 * Expands a given rectangle to the next tile boundary. Note, this will
 * expand the rectangle if it is already on tile boundaries.
 */
static CSSRect ExpandDisplayPortToTileBoundaries(
  const CSSRect& aDisplayPort,
  const CSSToLayerScale& aLayerPixelsPerCSSPixel)
{
  // Convert the given rect to layer coordinates so we can inflate to tile
  // boundaries (layer space corresponds to texture pixel space here).
  LayerRect displayPortInLayerSpace = aDisplayPort * aLayerPixelsPerCSSPixel;

  // Inflate the rectangle by 1 so that we always push to the next tile
  // boundary. This is desirable to stop from having a rectangle with a
  // moving origin occasionally being smaller when it coincidentally lines
  // up to tile boundaries.
  displayPortInLayerSpace.Inflate(1);

  // Now nudge the rectangle to the nearest equal or larger tile boundary.
  gfxFloat left = TILEDLAYERBUFFER_TILE_SIZE
    * floor(displayPortInLayerSpace.x / TILEDLAYERBUFFER_TILE_SIZE);
  gfxFloat top = TILEDLAYERBUFFER_TILE_SIZE
    * floor(displayPortInLayerSpace.y / TILEDLAYERBUFFER_TILE_SIZE);
  gfxFloat right = TILEDLAYERBUFFER_TILE_SIZE
    * ceil(displayPortInLayerSpace.XMost() / TILEDLAYERBUFFER_TILE_SIZE);
  gfxFloat bottom = TILEDLAYERBUFFER_TILE_SIZE
    * ceil(displayPortInLayerSpace.YMost() / TILEDLAYERBUFFER_TILE_SIZE);

  displayPortInLayerSpace = LayerRect(left, top, right - left, bottom - top);
  CSSRect displayPort = displayPortInLayerSpace / aLayerPixelsPerCSSPixel;

  return displayPort;
}

static void
MaybeAlignAndClampDisplayPort(mozilla::layers::FrameMetrics& aFrameMetrics,
                              const CSSPoint& aActualScrollOffset)
{
  // Correct the display-port by the difference between the requested scroll
  // offset and the resulting scroll offset after setting the requested value.
  CSSRect& displayPort = aFrameMetrics.mDisplayPort;
  displayPort += aFrameMetrics.mScrollOffset - aActualScrollOffset;

  // Expand the display port to the next tile boundaries, if tiled thebes layers
  // are enabled.
  if (gfxPrefs::LayersTilesEnabled()) {
    // We don't use LayersPixelsPerCSSPixel() here as mCumulativeResolution on
    // this FrameMetrics may be incorrect (and is about to be reset by mZoom).
    displayPort =
      ExpandDisplayPortToTileBoundaries(displayPort + aActualScrollOffset,
                                        aFrameMetrics.mZoom *
                                        ScreenToLayerScale(1.0))
      - aActualScrollOffset;
  }

  // Finally, clamp the display port to the expanded scrollable rect.
  CSSRect scrollableRect = aFrameMetrics.GetExpandedScrollableRect();
  displayPort = scrollableRect.Intersect(displayPort + aActualScrollOffset)
    - aActualScrollOffset;
}

static void
RecenterDisplayPort(mozilla::layers::FrameMetrics& aFrameMetrics)
{
    CSSRect compositionBounds(aFrameMetrics.CalculateCompositedRectInCssPixels());
    aFrameMetrics.mDisplayPort.x = (compositionBounds.width - aFrameMetrics.mDisplayPort.width) / 2;
    aFrameMetrics.mDisplayPort.y = (compositionBounds.height - aFrameMetrics.mDisplayPort.height) / 2;
}

static CSSPoint
ScrollFrameTo(nsIScrollableFrame* aFrame, const CSSPoint& aPoint, bool& aSuccessOut)
{
  aSuccessOut = false;

  if (!aFrame) {
    return aPoint;
  }

  CSSPoint targetScrollPosition = aPoint;

  // If the frame is overflow:hidden on a particular axis, we don't want to allow
  // user-driven scroll on that axis. Simply set the scroll position on that axis
  // to whatever it already is. Note that this will leave the APZ's async scroll
  // position out of sync with the gecko scroll position, but APZ can deal with that
  // (by design). Note also that when we run into this case, even if both axes
  // have overflow:hidden, we want to set aSuccessOut to true, so that the displayport
  // follows the async scroll position rather than the gecko scroll position.
  CSSPoint geckoScrollPosition = CSSPoint::FromAppUnits(aFrame->GetScrollPosition());
  if (aFrame->GetScrollbarStyles().mVertical == NS_STYLE_OVERFLOW_HIDDEN) {
    targetScrollPosition.y = geckoScrollPosition.y;
  }
  if (aFrame->GetScrollbarStyles().mHorizontal == NS_STYLE_OVERFLOW_HIDDEN) {
    targetScrollPosition.x = geckoScrollPosition.x;
  }

  // If the scrollable frame is currently in the middle of an async or smooth
  // scroll then we don't want to interrupt it (see bug 961280).
  // Also if the scrollable frame got a scroll request from something other than us
  // since the last layers update, then we don't want to push our scroll request
  // because we'll clobber that one, which is bad.
  if (!aFrame->IsProcessingAsyncScroll() &&
     (!aFrame->OriginOfLastScroll() || aFrame->OriginOfLastScroll() == nsGkAtoms::apz)) {
    aFrame->ScrollToCSSPixelsApproximate(targetScrollPosition, nsGkAtoms::apz);
    geckoScrollPosition = CSSPoint::FromAppUnits(aFrame->GetScrollPosition());
    aSuccessOut = true;
  }
  // Return the final scroll position after setting it so that anything that relies
  // on it can have an accurate value. Note that even if we set it above re-querying it
  // is a good idea because it may have gotten clamped or rounded.
  return geckoScrollPosition;
}

void
APZCCallbackHelper::UpdateRootFrame(nsIDOMWindowUtils* aUtils,
                                    FrameMetrics& aMetrics)
{
    // Precondition checks
    MOZ_ASSERT(aUtils);
    if (aMetrics.mScrollId == FrameMetrics::NULL_SCROLL_ID) {
        return;
    }

    // Set the scroll port size, which determines the scroll range. For example if
    // a 500-pixel document is shown in a 100-pixel frame, the scroll port length would
    // be 100, and gecko would limit the maximum scroll offset to 400 (so as to prevent
    // overscroll). Note that if the content here was zoomed to 2x, the document would
    // be 1000 pixels long but the frame would still be 100 pixels, and so the maximum
    // scroll range would be 900. Therefore this calculation depends on the zoom applied
    // to the content relative to the container.
    CSSSize scrollPort = CSSSize(aMetrics.CalculateCompositedRectInCssPixels().Size());
    aUtils->SetScrollPositionClampingScrollPortSize(scrollPort.width, scrollPort.height);

    // Scroll the window to the desired spot
    nsIScrollableFrame* sf = nsLayoutUtils::FindScrollableFrameFor(aMetrics.mScrollId);
    bool scrollUpdated = false;
    CSSPoint actualScrollOffset = ScrollFrameTo(sf, aMetrics.mScrollOffset, scrollUpdated);

    if (!scrollUpdated) {
      // For whatever reason we couldn't update the scroll offset on the scroll frame,
      // which means the data APZ used for its displayport calculation is stale. Fall
      // back to a sane default behaviour. Note that we don't tile-align the recentered
      // displayport because tile-alignment depends on the scroll position, and the
      // scroll position here is out of our control. See bug 966507 comment 21 for a
      // more detailed explanation.
      RecenterDisplayPort(aMetrics);
    }

    // Correct the display port due to the difference between mScrollOffset and the
    // actual scroll offset, possibly align it to tile boundaries (if tiled layers are
    // enabled), and clamp it to the scrollable rect.
    MaybeAlignAndClampDisplayPort(aMetrics, actualScrollOffset);

    aMetrics.mScrollOffset = actualScrollOffset;

    // The mZoom variable on the frame metrics stores the CSS-to-screen scale for this
    // frame. This scale includes all of the (cumulative) resolutions set on the presShells
    // from the root down to this frame. However, when setting the resolution, we only
    // want the piece of the resolution that corresponds to this presShell, rather than
    // all of the cumulative stuff, so we need to divide out the parent resolutions.
    // Finally, we multiply by a ScreenToLayerScale of 1.0f because the goal here is to
    // take the async zoom calculated by the APZC and tell gecko about it (turning it into
    // a "sync" zoom) which will update the resolution at which the layer is painted.
    ParentLayerToLayerScale presShellResolution =
        aMetrics.mZoom
        / aMetrics.mDevPixelsPerCSSPixel
        / aMetrics.GetParentResolution()
        * ScreenToLayerScale(1.0f);
    aUtils->SetResolution(presShellResolution.scale, presShellResolution.scale);

    // Finally, we set the displayport.
    nsCOMPtr<nsIContent> content = nsLayoutUtils::FindContentFor(aMetrics.mScrollId);
    if (!content) {
        return;
    }
    nsCOMPtr<nsIDOMElement> element = do_QueryInterface(content);
    if (!element) {
        return;
    }
    aUtils->SetDisplayPortForElement(aMetrics.mDisplayPort.x,
                                     aMetrics.mDisplayPort.y,
                                     aMetrics.mDisplayPort.width,
                                     aMetrics.mDisplayPort.height,
                                     element, 0);
}

void
APZCCallbackHelper::UpdateSubFrame(nsIContent* aContent,
                                   FrameMetrics& aMetrics)
{
    // Precondition checks
    MOZ_ASSERT(aContent);
    if (aMetrics.mScrollId == FrameMetrics::NULL_SCROLL_ID) {
        return;
    }

    nsCOMPtr<nsIDOMWindowUtils> utils = GetDOMWindowUtils(aContent);
    if (!utils) {
        return;
    }

    // We currently do not support zooming arbitrary subframes. They can only
    // be scrolled, so here we only have to set the scroll position and displayport.

    nsIScrollableFrame* sf = nsLayoutUtils::FindScrollableFrameFor(aMetrics.mScrollId);
    bool scrollUpdated = false;
    CSSPoint actualScrollOffset = ScrollFrameTo(sf, aMetrics.mScrollOffset, scrollUpdated);

    nsCOMPtr<nsIDOMElement> element = do_QueryInterface(aContent);
    if (element) {
        if (!scrollUpdated) {
            RecenterDisplayPort(aMetrics);
        }
        MaybeAlignAndClampDisplayPort(aMetrics, actualScrollOffset);
        utils->SetDisplayPortForElement(aMetrics.mDisplayPort.x,
                                        aMetrics.mDisplayPort.y,
                                        aMetrics.mDisplayPort.width,
                                        aMetrics.mDisplayPort.height,
                                        element, 0);
    }

    aMetrics.mScrollOffset = actualScrollOffset;
}

already_AddRefed<nsIDOMWindowUtils>
APZCCallbackHelper::GetDOMWindowUtils(const nsIDocument* aDoc)
{
    nsCOMPtr<nsIDOMWindowUtils> utils;
    nsCOMPtr<nsIDOMWindow> window = aDoc->GetDefaultView();
    if (window) {
        utils = do_GetInterface(window);
    }
    return utils.forget();
}

already_AddRefed<nsIDOMWindowUtils>
APZCCallbackHelper::GetDOMWindowUtils(const nsIContent* aContent)
{
    nsCOMPtr<nsIDOMWindowUtils> utils;
    nsIDocument* doc = aContent->GetCurrentDoc();
    if (doc) {
        utils = GetDOMWindowUtils(doc);
    }
    return utils.forget();
}

bool
APZCCallbackHelper::GetScrollIdentifiers(const nsIContent* aContent,
                                         uint32_t* aPresShellIdOut,
                                         FrameMetrics::ViewID* aViewIdOut)
{
    if (!aContent || !nsLayoutUtils::FindIDFor(aContent, aViewIdOut)) {
        return false;
    }
    nsCOMPtr<nsIDOMWindowUtils> utils = GetDOMWindowUtils(aContent);
    return utils && (utils->GetPresShellId(aPresShellIdOut) == NS_OK);
}

class AcknowledgeScrollUpdateEvent : public nsRunnable
{
    typedef mozilla::layers::FrameMetrics::ViewID ViewID;

public:
    AcknowledgeScrollUpdateEvent(const ViewID& aScrollId, const uint32_t& aScrollGeneration)
        : mScrollId(aScrollId)
        , mScrollGeneration(aScrollGeneration)
    {
    }

    NS_IMETHOD Run() {
        MOZ_ASSERT(NS_IsMainThread());

        nsIScrollableFrame* sf = nsLayoutUtils::FindScrollableFrameFor(mScrollId);
        if (sf) {
            sf->ResetOriginIfScrollAtGeneration(mScrollGeneration);
        }

        return NS_OK;
    }

protected:
    ViewID mScrollId;
    uint32_t mScrollGeneration;
};

void
APZCCallbackHelper::AcknowledgeScrollUpdate(const FrameMetrics::ViewID& aScrollId,
                                            const uint32_t& aScrollGeneration)
{
    nsCOMPtr<nsIRunnable> r1 = new AcknowledgeScrollUpdateEvent(aScrollId, aScrollGeneration);
    if (!NS_IsMainThread()) {
        NS_DispatchToMainThread(r1);
    } else {
        r1->Run();
    }
}

}
}
