#pragma once
#include "XPLMDefs.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef int    (*XPLMGetDatai_f) (void *);
typedef void   (*XPLMSetDatai_f) (void *, int);
typedef float  (*XPLMGetDataf_f) (void *);
typedef void   (*XPLMSetDataf_f) (void *, float);
typedef double (*XPLMGetDatad_f) (void *);
typedef void   (*XPLMSetDatad_f) (void *, double);
typedef int    (*XPLMGetDatavi_f)(void *, int *,   int, int);
typedef void   (*XPLMSetDatavi_f)(void *, int *,   int, int);
typedef int    (*XPLMGetDatavf_f)(void *, float *,  int, int);
typedef void   (*XPLMSetDatavf_f)(void *, float *,  int, int);
typedef int    (*XPLMGetDatab_f) (void *, void *,  int, int);
typedef void   (*XPLMSetDatab_f) (void *, void *,  int, int);

XPLMDataRef XPLMFindDataRef         (const char *inDataName);
int         XPLMGetDatai             (XPLMDataRef inDataRef);
void        XPLMSetDatai             (XPLMDataRef inDataRef, int inValue);
float       XPLMGetDataf             (XPLMDataRef inDataRef);
void        XPLMSetDataf             (XPLMDataRef inDataRef, float inValue);
double      XPLMGetDatad             (XPLMDataRef inDataRef);
void        XPLMSetDatad             (XPLMDataRef inDataRef, double inValue);
int         XPLMGetDatavf            (XPLMDataRef inDataRef, float *outValues, int inOffset, int inMax);
void        XPLMSetDatavf            (XPLMDataRef inDataRef, float *inValues,  int inOffset, int inCount);

XPLMDataRef XPLMRegisterDataAccessor(
    const char *inDataName, XPLMDataTypeID inDataType, int inIsWritable,
    XPLMGetDatai_f, XPLMSetDatai_f,
    XPLMGetDataf_f, XPLMSetDataf_f,
    XPLMGetDatad_f, XPLMSetDatad_f,
    XPLMGetDatavi_f, XPLMSetDatavi_f,
    XPLMGetDatavf_f, XPLMSetDatavf_f,
    XPLMGetDatab_f,  XPLMSetDatab_f,
    void *inReadRefcon, void *inWriteRefcon);

void XPLMUnregisterDataAccessor(XPLMDataRef inDataAccessor);

#ifdef __cplusplus
}
#endif
