#ifndef JSONTOOL_H
#define JSONTOOL_H

#include <string>
#include "singletion.h"
#include <boost/json.hpp>

class JsonTool : public Singleton<JsonTool>
{
    friend class Singleton<JsonTool>;

public:
    ~JsonTool();

    bool ReadJsonFile(const std::string& path, boost::json::value& out_val, std::string* err = nullptr);
    bool WriteJsonFile(const std::string& path, const boost::json::value& in_val, std::string* err = nullptr);

private:
    JsonTool() = default;
};

#endif // JSONTOOL_H