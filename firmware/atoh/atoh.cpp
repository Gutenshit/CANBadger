/**
 * @file   atoh.cpp
 * @brief  Convert an ASCII hex string to hexadecimal - seems like the 
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
 
#include "atoh.h"

template<typename T>
T atoh( char const *string )
{
    T value = 0;
    char ch = 0;
    uint8_t digit = 0, bail = sizeof(T) << 1;
    
    // loop until the string has ended
    while ( ( ch = *(string++) ) != 0 ) 
    {
        // service digits 0 - 9
        if ( ( ch >= '0' ) && ( ch <= '9' ) ) 
        {
            digit = ch - '0';
        }
        // and then lowercase a - f
        else if ( ( ch >= 'a' ) && ( ch <= 'f' ) ) 
        {
            digit = ch - 'a' + 10;
        }
        // and uppercase A - F
        else if ( ( ch >= 'A' ) && ( ch <= 'F' ) ) 
        {
            digit = ch - 'A' + 10;
        }
        // stopping where we are if an inapproprate value is found
        else 
        {
            break;
        }
        // if the return is 8, 16, or 32 bits - only parse that amount
        if ( bail-- == 0 )
        {
            break;
        }
        // and build the value now, preparing for the next pass
        value = (value << 4) + digit;
    }

    return value;
}

template uint8_t  atoh <uint8_t> (const char* string );
template uint16_t atoh <uint16_t>(const char* string );
template uint32_t atoh <uint32_t>(const char* string );
template uint64_t atoh <uint64_t>(const char* string );

