#include "config_load_internal.h"

void LoadThirdParty(ConfigReader& reader, AppConfig& config) {
    config.thirdParty.reshadeDllPath = reader.GetStr("ThirdParty", "reshade_dll_path", "");
    config.thirdParty.optiscalerDllPath = reader.GetStr("ThirdParty", "optiscaler_dll_path", "");
    config.thirdParty.specialkDllPath = reader.GetStr("ThirdParty", "specialk_dll_path", "");
}
