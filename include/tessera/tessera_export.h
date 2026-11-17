/* Symbol visibility for the Tessera C ABI. Hand-written on purpose: no build-time generation. */
#ifndef TESSERA_EXPORT_H
#define TESSERA_EXPORT_H

#if defined(TESSERA_STATIC)
#define TESSERA_API
#elif defined(_WIN32)
#if defined(TESSERA_BUILDING)
#define TESSERA_API __declspec(dllexport)
#else
#define TESSERA_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define TESSERA_API __attribute__((visibility("default")))
#else
#define TESSERA_API
#endif

#if defined(_WIN32)
#define TESSERA_CALL __cdecl
#else
#define TESSERA_CALL
#endif

#endif /* TESSERA_EXPORT_H */
