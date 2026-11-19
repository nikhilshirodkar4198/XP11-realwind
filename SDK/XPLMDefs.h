#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef void * XPLMDataRef;
typedef int    XPLMPluginID;
typedef int    XPLMDataTypeID;

#define xplmType_Int    1
#define xplmType_Float  2
#define xplmType_Double 4

#if IBM
#  define PLUGIN_API __declspec(dllexport)
#else
#  define PLUGIN_API
#endif

#ifdef __cplusplus
}
#endif
