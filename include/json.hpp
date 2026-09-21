#ifndef FPM_JSON_HPP
#define FPM_JSON_HPP

#include <string>
#include <vector>
#include <map>
#include <memory>

class JsonValue
{
public:
    enum Type
    {
        NUL,
        BOOLEAN,
        NUMBER,
        STRING,
        ARRAY,
        OBJECT
    };

    Type type = NUL;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    bool is_null() const { return type == NUL; }
    bool is_array() const { return type == ARRAY; }
    bool is_object() const { return type == OBJECT; }
    bool is_string() const { return type == STRING; }

    const JsonValue *get(const std::string &key) const
    {
        auto it = obj.find(key);
        if (it == obj.end())
            return nullptr;
        return &it->second;
    }

    std::string str_value(const std::string &key, const std::string &def = "") const
    {
        const JsonValue *v = get(key);
        if (v && v->is_string())
            return v->str;
        return def;
    }
};

class JsonParser
{
public:
    explicit JsonParser(const std::string &text);

    bool parse();
    bool ok() const { return parse_ok; }
    const JsonValue &root() const { return root_; }
    std::string error() const { return error_msg; }

private:
    const std::string &src;
    size_t pos = 0;
    bool parse_ok = false;
    std::string error_msg;
    JsonValue root_;

    void skip_ws();
    bool parse_value(JsonValue &out);
    bool parse_object(JsonValue &out);
    bool parse_array(JsonValue &out);
    bool parse_string(std::string &out);
    bool parse_number(JsonValue &out);
    bool parse_literal(JsonValue &out);
};

bool json_unescape(const std::string &in, std::string &out);

#endif // FPM_JSON_HPP