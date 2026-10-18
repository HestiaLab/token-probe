#ifndef TOKENPROBE_EXPORT_H
#define TOKENPROBE_EXPORT_H

#if defined(_WIN32) && defined(TOKENPROBE_SHARED)
#if defined(TOKENPROBE_BUILDING_LIBRARY)
#define TOKENPROBE_API __declspec(dllexport)
#else
#define TOKENPROBE_API __declspec(dllimport)
#endif
#else
#define TOKENPROBE_API
#endif

#endif /* TOKENPROBE_EXPORT_H */
