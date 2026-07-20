#pragma once

#ifdef _WIN32
#include "erwt3d/posix_compat.hpp"
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#endif
