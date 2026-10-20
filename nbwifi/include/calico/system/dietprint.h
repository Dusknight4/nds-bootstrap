// nbwifi: stand-in for calico's dietprint: debug lines go to nbwifi's log
// (see nbwifiLog in nbwifi.h)
// SPDX-License-Identifier: ZPL-2.1
#pragma once

void nbwLogf(const char* fmt, ...);
#define dietPrint nbwLogf
