// snd_user (TTT2): the AXFX effects (reverb, chorus, delay), the MIX mixer and AXART articulation.
// Effects pass their aux bus through unchanged; MIX keeps no state (voices keep the device mix the
// game sets with AX directly). Enough for the game to set up its sound system.
#include "../runtime.h"

HLE(snd_user, AXARTInit) {}
HLE(snd_user, AXARTQuit) {}
HLE(snd_user, AXFXChorusCallback) {}
HLE(snd_user, AXFXChorusExpCallback) {}
HLE(snd_user, AXFXChorusExpCallbackDpl2) {}
HLE(snd_user, AXFXChorusExpGetMemSize) { ret(c, 0x100); }
HLE(snd_user, AXFXChorusExpGetMemSizeDpl2) { ret(c, 0x100); }
HLE(snd_user, AXFXChorusExpInit) { ret(c, 1); }
HLE(snd_user, AXFXChorusExpInitDpl2) { ret(c, 1); }
HLE(snd_user, AXFXChorusExpSettings) { ret(c, 1); }
HLE(snd_user, AXFXChorusExpSettingsDpl2) { ret(c, 1); }
HLE(snd_user, AXFXChorusExpShutdown) {}
HLE(snd_user, AXFXChorusExpShutdownDpl2) {}
HLE(snd_user, AXFXChorusGetMemSize) { ret(c, 0x100); }
HLE(snd_user, AXFXChorusInit) { ret(c, 1); }
HLE(snd_user, AXFXChorusSettings) { ret(c, 1); }
HLE(snd_user, AXFXChorusShutdown) {}
HLE(snd_user, AXFXDelayCallback) {}
HLE(snd_user, AXFXDelayExpCallback) {}
HLE(snd_user, AXFXDelayExpCallbackDpl2) {}
HLE(snd_user, AXFXDelayExpGetMemSize) { ret(c, 0x100); }
HLE(snd_user, AXFXDelayExpGetMemSizeDpl2) { ret(c, 0x100); }
HLE(snd_user, AXFXDelayExpInit) { ret(c, 1); }
HLE(snd_user, AXFXDelayExpInitDpl2) { ret(c, 1); }
HLE(snd_user, AXFXDelayExpSettings) { ret(c, 1); }
HLE(snd_user, AXFXDelayExpSettingsDpl2) { ret(c, 1); }
HLE(snd_user, AXFXDelayExpShutdown) {}
HLE(snd_user, AXFXDelayExpShutdownDpl2) {}
HLE(snd_user, AXFXDelayGetMemSize) { ret(c, 0x100); }
HLE(snd_user, AXFXDelayInit) { ret(c, 1); }
HLE(snd_user, AXFXDelaySettings) { ret(c, 1); }
HLE(snd_user, AXFXDelayShutdown) {}
HLE(snd_user, AXFXReverbHiCallback) {}
HLE(snd_user, AXFXReverbHiCallbackDpl2) {}
HLE(snd_user, AXFXReverbHiExpCallback) {}
HLE(snd_user, AXFXReverbHiExpCallbackDpl2) {}
HLE(snd_user, AXFXReverbHiExpGetMemSize) { ret(c, 0x100); }
HLE(snd_user, AXFXReverbHiExpGetMemSizeDpl2) { ret(c, 0x100); }
HLE(snd_user, AXFXReverbHiExpInit) { ret(c, 1); }
HLE(snd_user, AXFXReverbHiExpInitDpl2) { ret(c, 1); }
HLE(snd_user, AXFXReverbHiExpSettings) { ret(c, 1); }
HLE(snd_user, AXFXReverbHiExpSettingsDpl2) { ret(c, 1); }
HLE(snd_user, AXFXReverbHiExpShutdown) {}
HLE(snd_user, AXFXReverbHiExpShutdownDpl2) {}
HLE(snd_user, AXFXReverbHiGetMemSize) { ret(c, 0x100); }
HLE(snd_user, AXFXReverbHiGetMemSizeDpl2) { ret(c, 0x100); }
HLE(snd_user, AXFXReverbHiInit) { ret(c, 1); }
HLE(snd_user, AXFXReverbHiInitDpl2) { ret(c, 1); }
HLE(snd_user, AXFXReverbHiSettings) { ret(c, 1); }
HLE(snd_user, AXFXReverbHiSettingsDpl2) { ret(c, 1); }
HLE(snd_user, AXFXReverbHiShutdown) {}
HLE(snd_user, AXFXReverbHiShutdownDpl2) {}
HLE(snd_user, AXFXReverbStdCallback) {}
HLE(snd_user, AXFXReverbStdExpCallback) {}
HLE(snd_user, AXFXReverbStdExpCallbackDpl2) {}
HLE(snd_user, AXFXReverbStdExpGetMemSize) { ret(c, 0x100); }
HLE(snd_user, AXFXReverbStdExpGetMemSizeDpl2) { ret(c, 0x100); }
HLE(snd_user, AXFXReverbStdExpInit) { ret(c, 1); }
HLE(snd_user, AXFXReverbStdExpInitDpl2) { ret(c, 1); }
HLE(snd_user, AXFXReverbStdExpSettings) { ret(c, 1); }
HLE(snd_user, AXFXReverbStdExpSettingsDpl2) { ret(c, 1); }
HLE(snd_user, AXFXReverbStdExpShutdown) {}
HLE(snd_user, AXFXReverbStdExpShutdownDpl2) {}
HLE(snd_user, AXFXReverbStdGetMemSize) { ret(c, 0x100); }
HLE(snd_user, AXFXReverbStdInit) { ret(c, 1); }
HLE(snd_user, AXFXReverbStdSettings) { ret(c, 1); }
HLE(snd_user, AXFXReverbStdShutdown) {}
HLE(snd_user, MIXAssignChannel) {}
HLE(snd_user, MIXInit) {}
HLE(snd_user, MIXInitInputControl) {}
HLE(snd_user, MIXQuit) {}
HLE(snd_user, MIXReleaseChannel) {}
HLE(snd_user, MIXSetDeviceSoundMode) {}
HLE(snd_user, MIXUpdateSettings) {}
