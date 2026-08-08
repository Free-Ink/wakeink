#pragma once

// Swipe classification over the SDK's raw gesture report. InputManager latches
// a swipe on release as normalized panel-native start/end points ("callers map
// orientation"); this maps both endpoints through the same touchToLogical
// transform taps use (device.touchOrientation + the app's mount flips), then
// classifies by the dominant axis. Swipes never double as taps — the tap
// classifier's slop gate already rejected the gesture.

#include <FreeInkUI.h>
#include <InputManager.h>

namespace wakeink {

struct Swipe {
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;
  explicit operator bool() const { return up || down || left || right; }
};

inline Swipe readSwipe(const InputManager& input, const freeink::ui::DeviceContext& device,
                       const bool flipX, const bool flipY) {
  namespace ui = freeink::ui;
  Swipe s;
  float nxs = 0, nys = 0, nxe = 0, nye = 0;
  if (!input.hasTouch() || !input.wasSwipe(nxs, nys, nxe, nye)) return s;
  const ui::Point a = ui::touchToLogical(device, nxs, nys, flipX, flipY);
  const ui::Point b = ui::touchToLogical(device, nxe, nye, flipX, flipY);
  const int dx = b.x - a.x;
  const int dy = b.y - a.y;
  // Dominant axis wins; a sixth of the screen filters out sloppy taps that
  // slipped past the SDK's own swipe threshold.
  if (abs(dx) >= abs(dy)) {
    s.left = dx <= -(device.width / 6);
    s.right = dx >= device.width / 6;
  } else {
    s.up = dy <= -(device.height / 6);
    s.down = dy >= device.height / 6;
  }
  return s;
}

}  // namespace wakeink
