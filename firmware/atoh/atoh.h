/**
 * @file   atoh.h
 * @brief  Convert a hex formatted ASCII hex string to hex. Seems like the 
 *  std library should include this... maybe it does. Just couldn't find it
 * @author  sam grove
 * @version 1.0
 *
 * Copyright (c) 2013
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#ifndef ATOH_H
#define ATOH_H

#include <stdint.h>

/** Convert a hex formatted ASCII string to it's binary equivenent
 *
 * Example:
 * @code
 *  #include "mbed.h"
 *  #include "atoh.h"
 * 
 *  int main()
 *  {
 *      uint64_t result = atoh <uint64_t> ("0123456789abcdef" );
 *      uint32_t lo = result & 0x00000000ffffffff;
 *      uint32_t hi = (result >> 32);
 *      printf( "0x%08X%08X\n", hi, lo );
 *      printf( "0x%08X\n", atoh <uint32_t> ( "12345678" ) );
 *      printf( "0x%04X\n", atoh <uint16_t> ( "1234" ) );
 *      printf( "0x%02X\n", atoh <uint8_t> ( "12" ) );
 *  }
 * @endcode
 */

/** A templated method for ascii to hex conversions. Supported types are:
 *  uint8_t, uint16_t, uint32_t and uint64_t
 *  @param string - An ascii string of hex digits
 *  @returns The binary equivelant of the string
 */
template<typename T>
T atoh( char const *string );

#endif

