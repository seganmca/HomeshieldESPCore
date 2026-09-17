#pragma once

#include <Arduino.h>

// ============================================================
// JsonLite
// ============================================================
//
// Reads one field out of a flat JSON object. Moved out of HomeShieldClass in
// milestone 37 so the BLE provisioning messages and the registration response
// can use the same reader. A handful of known flat fields do not justify a JSON
// library in every sketch's flash budget.
// ============================================================

namespace JsonLite
{
    // The start index of the value that follows "field": in json, or -1.
    inline int FindValue(
        const String& json,
        const String& field)
    {
        String needle =
            "\"" + field + "\"";

        int at =
            json.indexOf(needle);

        if (at < 0) return -1;

        int colon =
            json.indexOf(':', at + needle.length());

        if (colon < 0) return -1;

        for (unsigned int i = colon + 1;
             i < json.length();
             i++)
        {
            char c = json[i];

            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;

            return i;
        }

        return -1;
    }


    inline bool ReadString(
        const String& json,
        const String& field,
        String& value)
    {
        value = "";

        int start =
            FindValue(json, field);

        if (start < 0 || json[start] != '"') return false;

        String result;

        for (unsigned int i = start + 1;
             i < json.length();
             i++)
        {
            char c = json[i];

            if (c == '\\')
            {
                if (i + 1 >= json.length()) return false;

                char escaped = json[i + 1];

                switch (escaped)
                {
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/';  break;
                    case 'n':  result += '\n'; break;
                    case 'r':  result += '\r'; break;
                    case 't':  result += '\t'; break;

                    // Anything else, including \uXXXX, is kept verbatim
                    // rather than failing the whole message.
                    default:   result += escaped; break;
                }

                i++;

                continue;
            }

            if (c == '"')
            {
                value = result;

                return true;
            }

            result += c;
        }

        // Unterminated string.
        return false;
    }


    inline bool ReadLong(
        const String& json,
        const String& field,
        long& value)
    {
        value = 0;

        int start =
            FindValue(json, field);

        if (start < 0) return false;

        unsigned int end = start;

        if (end < json.length() && json[end] == '-') end++;

        unsigned int digits = end;

        while (end < json.length() && isDigit(json[end])) end++;

        if (end == digits) return false;

        value = json.substring(start, end).toInt();

        return true;
    }


    // Escapes the characters that would break a JSON string literal.
    inline String Escape(
        const String& value)
    {
        String out;

        out.reserve(value.length() + 8);

        for (unsigned int i = 0;
             i < value.length();
             i++)
        {
            char c = value[i];

            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;

                default:
                    // Control characters are not legal raw in a JSON string.
                    if ((unsigned char)c >= 0x20) out += c;
                    break;
            }
        }

        return out;
    }
}
