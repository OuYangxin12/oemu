/*
 * Portability macros shared by every public oemu header.
 *
 * OEMU_BEGIN_DECLS / OEMU_END_DECLS are what make this pure C library usable
 * from the C++ GoogleTest translation units: without extern "C" the test code
 * would emit mangled symbol references and fail to link.
 */
#ifndef OEMU_MACROS_H
#define OEMU_MACROS_H

#ifdef __cplusplus
#  define OEMU_BEGIN_DECLS extern "C" {
#  define OEMU_END_DECLS   }
#else
#  define OEMU_BEGIN_DECLS
#  define OEMU_END_DECLS
#endif

/* Warn when a status return value is ignored at a call site. */
#if defined(__GNUC__) || defined(__clang__)
#  define OEMU_NODISCARD __attribute__((warn_unused_result))
#  define OEMU_PRINTF(fmt_index, first_arg) \
     __attribute__((format(printf, fmt_index, first_arg)))
#else
#  define OEMU_NODISCARD
#  define OEMU_PRINTF(fmt_index, first_arg)
#endif

#endif /* OEMU_MACROS_H */
