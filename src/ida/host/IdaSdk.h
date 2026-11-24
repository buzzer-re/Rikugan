#pragma once
// The IDA SDK headers every IDA translation unit uses. Include Qt and STL headers before this one: pro.h
// redirects stdio/string functions that the standard headers declare.
#define USE_STANDARD_FILE_FUNCTIONS
#define USE_DANGEROUS_FUNCTIONS
// pro.h defines it from __LINUX__/__MAC__; the build's -D__UNIX__=1 would make that a redefinition warning
#undef __UNIX__

// The SDK headers do not build cleanly with -Wall -Wextra; their warnings are not ours to fix.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <pro.h>
#include <ida.hpp>
#include <idp.hpp>
#include <auto.hpp>
#include <bytes.hpp>
#include <entry.hpp>
#include <expr.hpp>
#include <funcs.hpp>
#include <gdl.hpp>
#include <hexrays.hpp>
#include <kernwin.hpp>
#include <lines.hpp>
#include <loader.hpp>
#include <nalt.hpp>
#include <name.hpp>
#include <netnode.hpp>
#include <segment.hpp>
#include <strlist.hpp>
#include <typeinf.hpp>
#include <ua.hpp>
#include <xref.hpp>

#pragma GCC diagnostic pop
