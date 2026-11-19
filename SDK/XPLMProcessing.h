#pragma once
#include "XPLMDefs.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef float (*XPLMFlightLoop_f)(float inElapsedSinceLastCall,
                                   float inElapsedTimeSinceLastFlightLoop,
                                   int   inCounter,
                                   void *inRefcon);

void XPLMRegisterFlightLoopCallback  (XPLMFlightLoop_f inFlightLoop, float inInterval, void *inRefcon);
void XPLMUnregisterFlightLoopCallback(XPLMFlightLoop_f inFlightLoop, void *inRefcon);

#ifdef __cplusplus
}
#endif
