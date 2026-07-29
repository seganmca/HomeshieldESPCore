#pragma once

#include <Arduino.h>

class HttpService
{
public:

    String Post(
        const String& url,
        const String& json);
};