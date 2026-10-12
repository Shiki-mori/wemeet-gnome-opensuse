#pragma once

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xcomposite.h>

#include <cstdio>
#include <dlfcn.h>

#include <functional>
#include <string>

template <class FType>
struct OriginalFunction{

  OriginalFunction(std::string const& funcName, std::string const& libN){
    func = (FType*)dlsym(RTLD_NEXT, funcName.c_str());
    if(func == nullptr){
      dl_handle = dlopen(libN.c_str(), RTLD_LAZY);
      if(dl_handle == nullptr){
        fprintf(stderr, "Failed to open library %s\n", libN.c_str());
        return;
      }
      func = (FType*)dlsym(dl_handle, funcName.c_str());
      if(func == nullptr){
        fprintf(stderr, "Failed to find function %s\n", funcName.c_str());
      }
    }
  }

  ~OriginalFunction(){
    if(dl_handle != nullptr){
      dlclose(dl_handle);
    }
  }

  template <class... Args>
  auto operator()(Args... args){
    return func(args...);
  }

  std::function<FType> func;
  void* dl_handle{nullptr};
  
}; // struct OriginalFunction

using XShmAttachType = Bool(Display*, XShmSegmentInfo*);
inline auto XShmAttachFunc = OriginalFunction<XShmAttachType>{"XShmAttach", "libXext.so"};

using XShmDetachType = Bool(Display*, XShmSegmentInfo*);
inline auto XShmDetachFunc = OriginalFunction<XShmDetachType>{"XShmDetach", "libXext.so"};

using XShmGetImageType = Bool(Display*, Drawable, XImage*, int, int, unsigned long);
inline auto XShmGetImageFunc = OriginalFunction<XShmGetImageType>{"XShmGetImage", "libXext.so"};

using XGetImageType = XImage*(Display*, Drawable, int, int, unsigned int, unsigned int, unsigned long, int);
inline auto XGetImageFunc = OriginalFunction<XGetImageType>{"XGetImage", "libX11.so.6"};

using XDamageQueryExtensionType = Bool(Display*, int*, int*);
inline auto XDamageQueryExtensionFunc = OriginalFunction<XDamageQueryExtensionType>{"XDamageQueryExtension", "libXdamage.so"};

using XCopyAreaType = int(Display*, Drawable, Drawable, GC, int, int, unsigned int, unsigned int, int, int);
inline auto XCopyAreaFunc = OriginalFunction<XCopyAreaType>{"XCopyArea", "libX11.so.6"};

using XShmQueryVersionType = Bool(Display*, int*, int*, Bool*);
inline auto XShmQueryVersionFunc = OriginalFunction<XShmQueryVersionType>{"XShmQueryVersion", "libXext.so"};

using XShmCreateImageType = XImage*(Display*, Visual*, unsigned int, int, char*, XShmSegmentInfo*, unsigned int, unsigned int);
inline auto XShmCreateImageFunc = OriginalFunction<XShmCreateImageType>{"XShmCreateImage", "libXext.so"};

using XShmCreatePixmapType = Pixmap(Display*, Drawable, char*, XShmSegmentInfo*, unsigned int, unsigned int, unsigned int);
inline auto XShmCreatePixmapFunc = OriginalFunction<XShmCreatePixmapType>{"XShmCreatePixmap", "libXext.so"};

using XDamageCreateType = Damage(Display*, Drawable, int);
inline auto XDamageCreateFunc = OriginalFunction<XDamageCreateType>{"XDamageCreate", "libXdamage.so"};

using XCompositeRedirectWindowType = void(Display*, Window, int);
inline auto XCompositeRedirectWindowFunc = OriginalFunction<XCompositeRedirectWindowType>{"XCompositeRedirectWindow", "libXcomposite.so"};

using XPutImageType = int(Display*, Drawable, GC, XImage*, int, int, int, int, unsigned int, unsigned int);
inline auto XPutImageFunc = OriginalFunction<XPutImageType>{"XPutImage", "libX11.so.6"};