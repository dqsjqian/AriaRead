// quickjs_msvc_shim.h - force-included (/FI) when compiling quickjs.c with MSVC.
//
// With the dynamic CRT (/MD), UCRT math functions are dllimport: their
// addresses are not compile-time constants, which breaks the static
// JSCFunctionListEntry tables in quickjs.c (C2099 at the array declaration).
// Redirect every math function referenced from those tables through static
// wrappers so the addresses become constants again.
#ifndef QUICKJS_MSVC_SHIM_H
#define QUICKJS_MSVC_SHIM_H

#include <math.h>

static double qjs_fabs(double x)   { return fabs(x);   }
static double qjs_floor(double x)  { return floor(x);  }
static double qjs_ceil(double x)   { return ceil(x);   }
static double qjs_sqrt(double x)   { return sqrt(x);   }
static double qjs_acos(double x)   { return acos(x);   }
static double qjs_asin(double x)   { return asin(x);   }
static double qjs_atan(double x)   { return atan(x);   }
static double qjs_atan2(double y, double x) { return atan2(y, x); }
static double qjs_cos(double x)    { return cos(x);    }
static double qjs_exp(double x)    { return exp(x);    }
static double qjs_log(double x)    { return log(x);    }
static double qjs_sin(double x)    { return sin(x);    }
static double qjs_tan(double x)    { return tan(x);    }
static double qjs_cosh(double x)   { return cosh(x);   }
static double qjs_sinh(double x)   { return sinh(x);   }
static double qjs_tanh(double x)   { return tanh(x);   }
static double qjs_cbrt(double x)   { return cbrt(x);   }
static double qjs_trunc(double x)  { return trunc(x);  }
static double qjs_acosh(double x)  { return acosh(x);  }
static double qjs_asinh(double x)  { return asinh(x);  }
static double qjs_atanh(double x)  { return atanh(x);  }
static double qjs_expm1(double x)  { return expm1(x);  }
static double qjs_log1p(double x)  { return log1p(x);  }
static double qjs_log2(double x)   { return log2(x);   }
static double qjs_log10(double x)  { return log10(x);  }

#define fabs   qjs_fabs
#define floor  qjs_floor
#define ceil   qjs_ceil
#define sqrt   qjs_sqrt
#define acos   qjs_acos
#define asin   qjs_asin
#define atan   qjs_atan
#define atan2  qjs_atan2
#define cos    qjs_cos
#define exp    qjs_exp
#define log    qjs_log
#define sin    qjs_sin
#define tan    qjs_tan
#define cosh   qjs_cosh
#define sinh   qjs_sinh
#define tanh   qjs_tanh
#define cbrt   qjs_cbrt
#define trunc  qjs_trunc
#define acosh  qjs_acosh
#define asinh  qjs_asinh
#define atanh  qjs_atanh
#define expm1  qjs_expm1
#define log1p  qjs_log1p
#define log2   qjs_log2
#define log10  qjs_log10

#endif /* QUICKJS_MSVC_SHIM_H */
