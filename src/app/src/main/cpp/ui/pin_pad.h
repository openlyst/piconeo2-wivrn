#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <functional>

// PIN entry state shared between streaming_client and the render thread.
// When the server requests a PIN, streaming_client sets gPinEntryRequested.
// The render thread shows a numpad overlay; when the user submits,
// gOnPinSubmit is called with the entered PIN.

extern std::atomic<bool> gPinEntryRequested;
extern std::function<void(const std::string &)> gOnPinSubmit;

// Called by streaming_client to request PIN entry from the UI.
void requestPinEntryUI();
// Called by the render thread when the user submits or cancels.
void submitPin(const std::string &pin);

// Build the PIN pad overlay vertices (panel-local metres).
// Returns true if the overlay is active (should be drawn).
bool buildPinPad(std::vector<float> &v, float cursorLx, float cursorLy,
                 bool clickEdge, bool cursorOnPanel);
