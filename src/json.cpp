#include "json.hpp"
#include <cctype>
#include <cstdlib>
#include <cmath>

JsonParser::JsonParser(const std::string &text) : src(text)
{
}

void JsonParser::skip_ws()
{
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
        ++pos;
}

bool json_unescape(const std::string &in, std::string &out)
{
    out.clear();
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] != '\\')
        {
            out += in[i];
            continue;
        }
        ++i;
        if (i >= in.size())
            return false;
        switch (in[i])
        {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'u':
            out += '?';
            i += 4;
            break;
        default:
            return false;
        }
    }
    return true;
}

bool JsonParser::parse_string(std::string &out)
{
    if (pos >= src.size() || src[pos] != '"')
        return false;
    ++pos;

    std::string raw;
    while (pos < src.size())
    {
        char c = src[pos];
        if (c == '\"')
        {
            ++pos;
            return json_unescape(raw, out);
        }
        if (c == '\\')
        {
            raw += c;
            ++pos;
            if (pos < src.size())
            {
                raw += src[pos];
                ++pos;
            }
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20)
            return false;
        raw += c;
        ++pos;
    }
    return false;
}

bool JsonParser::parse_number(JsonValue &out)
{
    size_t start = pos;
    if (pos < src.size() && (src[pos] == '-' || src[pos] == '+'))
        ++pos;
    while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' || src[pos] == '-' || src[pos] == '+'))
        ++pos;
    if (pos == start)
        return false;

    out.type = JsonValue::NUMBER;
    out.number = std::strtod(src.substr(start, pos - start).c_str(), nullptr);
    return true;
}

bool JsonParser::parse_literal(JsonValue &out)
{
    const std::string rest = src.substr(pos, 5);
    if (rest.rfind("true", 0) == 0)
    {
        out.type = JsonValue::BOOLEAN;
        out.boolean = true;
        pos += 4;
        return true;
    }
    if (rest.rfind("false", 0) == 0)
    {
        out.type = JsonValue::BOOLEAN;
        out.boolean = false;
        pos += 5;
        return true;
    }
    if (rest.rfind("null", 0) == 0)
    {
        out.type = JsonValue::NUL;
        pos += 4;
        return true;
    }
    return false;
}

bool JsonParser::parse_array(JsonValue &out)
{
    if (pos >= src.size() || src[pos] != '[')
        return false;
    ++pos;
    skip_ws();

    out.type = JsonValue::ARRAY;
    if (pos < src.size() && src[pos] == ']')
    {
        ++pos;
        return true;
    }

    while (pos < src.size())
    {
        skip_ws();
        JsonValue item;
        if (!parse_value(item))
            return false;
        out.arr.push_back(item);
        skip_ws();

        if (pos >= src.size())
            return false;
        if (src[pos] == ']')
        {
            ++pos;
            return true;
        }
        if (src[pos] == ',')
        {
            ++pos;
            continue;
        }
        return false;
    }
    return false;
}

bool JsonParser::parse_object(JsonValue &out)
{
    if (pos >= src.size() || src[pos] != '{')
        return false;
    ++pos;
    skip_ws();

    out.type = JsonValue::OBJECT;
    if (pos < src.size() && src[pos] == '}')
    {
        ++pos;
        return true;
    }

    while (pos < src.size())
    {
        skip_ws();
        std::string key;
        if (!parse_string(key))
            return false;
        skip_ws();
        if (pos >= src.size() || src[pos] != ':')
            return false;
        ++pos;
        skip_ws();
        JsonValue value;
        if (!parse_value(value))
            return false;
        out.obj[key] = value;
        skip_ws();

        if (pos >= src.size())
            return false;
        if (src[pos] == '}')
        {
            ++pos;
            return true;
        }
        if (src[pos] == ',')
        {
            ++pos;
            continue;
        }
        return false;
    }
    return false;
}

bool JsonParser::parse_value(JsonValue &out)
{
    skip_ws();
    if (pos >= src.size())
    {
        error_msg = "unexpected end of JSON";
        return false;
    }

    char c = src[pos];
    if (c == '{')
        return parse_object(out);
    if (c == '[')
        return parse_array(out);
    if (c == '"')
        return parse_string(out.str) && (out.type = JsonValue::STRING, true);
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
        return parse_number(out);
    return parse_literal(out);
}

bool JsonParser::parse()
{
    parse_ok = parse_value(root_);
    skip_ws();
    if (parse_ok && pos != src.size())
    {
        parse_ok = false;
        error_msg = "trailing garbage after JSON value";
    }
    return parse_ok;
}