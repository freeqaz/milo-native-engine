// ThermalState_Mac.mm -- the macOS thermal pressure level, for
// platform/ThermalState.cpp (milo::ThermalOsLevel). Objective-C++ because
// NSProcessInfo is an ObjC class.
//
// NSProcessInfoThermalState is Nominal = 0, Fair = 1, Serious = 2,
// Critical = 3 (Foundation/NSProcessInfo.h, macOS 10.10.3+), the numbering
// milo::ThermalLevel uses, so the value is returned as it is.

#import <Foundation/Foundation.h>

extern "C" int MiloMacThermalState() {
    return (int)[[NSProcessInfo processInfo] thermalState];
}
