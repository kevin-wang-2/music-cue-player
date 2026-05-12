// Defines all VST3 interface iid static members used by the MCP VST3 adapter.
// INIT_CLASS_IID causes DECLARE_CLASS_IID to emit const FUID::iid definitions.
// This file must be compiled in EXACTLY ONE translation unit.
#ifdef MCP_HAVE_VST3

#define INIT_CLASS_IID

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/gui/iplugview.h"

#endif // MCP_HAVE_VST3
