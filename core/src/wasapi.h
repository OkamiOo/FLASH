#pragma once

#include "flash/video.h"

#include <mmdeviceapi.h>

#include <string>
#include <vector>

namespace flash {

// Périphérique son dont le nom contient match (sans tenir compte de la casse) ; vide : celui
// de Windows. name reçoit son nom complet.
ComPtr<IMMDevice> find_audio_device(EDataFlow flow, const std::wstring &match, std::string *name = nullptr);
std::vector<std::string> list_audio_devices(EDataFlow flow);

} // namespace flash
