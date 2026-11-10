#include "stock_tools.h"

#include "mcp_server.h"
#include "music/music_api_client.h"

#include <esp_log.h>

#include <cJSON.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#define TAG "StockTools"

namespace {

std::string FmtNum(const cJSON* obj, const char* key, const char* fmt, const char* suffix = "") {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(item)) {
        return std::string();
    }
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, item->valuedouble);
    return std::string(buf) + suffix;
}

std::string FormatQuote(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return json;
    }
    const cJSON* name = cJSON_GetObjectItem(root, "name");
    const cJSON* code = cJSON_GetObjectItem(root, "code");
    const cJSON* price = cJSON_GetObjectItem(root, "price");
    const cJSON* chg = cJSON_GetObjectItem(root, "change");
    const cJSON* pct = cJSON_GetObjectItem(root, "change_pct");

    std::string out;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s(%s) 现价 ", cJSON_IsString(name) ? name->valuestring : "?",
             cJSON_IsString(code) ? code->valuestring : "?");
    out += buf;
    if (cJSON_IsNumber(price)) {
        snprintf(buf, sizeof(buf), "%.2f", price->valuedouble);
        out += buf;
    } else {
        out += "?";
    }
    if (cJSON_IsNumber(chg) && cJSON_IsNumber(pct)) {
        snprintf(buf, sizeof(buf), "，%s%.2f（%s%.2f%%）",
                 chg->valuedouble >= 0 ? "涨 " : "跌 ", chg->valuedouble,
                 pct->valuedouble >= 0 ? "+" : "", pct->valuedouble);
        out += buf;
    }
    out += "。";

    auto append = [&out](const std::string& label, const std::string& value) {
        if (!value.empty()) {
            out += label + value + "，";
        }
    };
    append("今开 ", FmtNum(root, "open", "%.2f"));
    append("最高 ", FmtNum(root, "high", "%.2f"));
    append("最低 ", FmtNum(root, "low", "%.2f"));
    append("昨收 ", FmtNum(root, "prev_close", "%.2f"));
    append("换手率 ", FmtNum(root, "turnover_pct", "%.2f", "%"));
    append("市盈率 ", FmtNum(root, "pe", "%.2f"));
    append("市净率 ", FmtNum(root, "pb", "%.2f"));
    append("总市值 ", FmtNum(root, "market_cap_yi", "%.1f", "亿"));
    append("成交额 ", FmtNum(root, "amount_wan", "%.0f", "万元"));
    const cJSON* tm = cJSON_GetObjectItem(root, "time");
    if (cJSON_IsString(tm) && strlen(tm->valuestring) >= 14) {
        out += std::string("行情时间 ") + tm->valuestring + "。";
    }
    if (!out.empty() && out.back() == '\xef') {
        // no-op guard, keep ascii-safe
    }
    cJSON_Delete(root);
    return out;
}

std::string FormatSearch(const std::string& json) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return json;
    }
    cJSON* items = cJSON_GetObjectItem(root, "items");
    std::string out;
    int idx = 0;
    if (cJSON_IsArray(items)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, items) {
            const cJSON* code = cJSON_GetObjectItem(it, "code");
            const cJSON* name = cJSON_GetObjectItem(it, "name");
            const cJSON* market = cJSON_GetObjectItem(it, "market");
            const cJSON* type = cJSON_GetObjectItem(it, "type");
            if (!cJSON_IsString(code) || !cJSON_IsString(name)) {
                continue;
            }
            char line[192];
            const char* mk = cJSON_IsString(market) ? market->valuestring : "";
            const char* tp = cJSON_IsString(type) ? type->valuestring : "";
            snprintf(line, sizeof(line), "%d) %s%s %s%s%s；", ++idx,
                     mk, code->valuestring, name->valuestring, tp[0] ? " " : "", tp);
            out += line;
            if (idx >= 6) {
                break;
            }
        }
    }
    cJSON_Delete(root);
    if (out.empty()) {
        return "未找到匹配的股票。";
    }
    return out + "查询行情请用 self.stock.quote，code 用上面括号里的市场+代码，例如 sh600519、hk00700、usNVDA。";
}

}  // namespace

void StockTools::Initialize() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    auto& mcp = McpServer::GetInstance();
    auto& api = MusicApiClient::GetInstance();

    mcp.AddTool(
        "self.stock.quote",
        "查询股票实时行情（现价、涨跌幅、今开/最高/最低、换手率、市盈率、市净率、总市值、成交额）。"
        "code 支持：A股 sh600519/sz000001 或 600519；港股 hk00700 或 0700.HK；美股 usNVDA 或 NVDA。",
        PropertyList({Property("code", kPropertyTypeString)}),
        [&api](const PropertyList& props) -> ReturnValue {
            std::string json;
            auto code = props["code"].value<std::string>();
            if (!api.StockQuote(code, &json)) {
                // 可能是名字：先搜一次，取第一个代码再查
                std::string search_json;
                cJSON* root = nullptr;
                if (api.StockSearch(code, &search_json)) {
                    root = cJSON_Parse(search_json.c_str());
                }
                std::string first_code;
                if (root != nullptr) {
                    cJSON* items = cJSON_GetObjectItem(root, "items");
                    if (cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) {
                        cJSON* it = cJSON_GetArrayItem(items, 0);
                        const cJSON* mkt = cJSON_GetObjectItem(it, "market");
                        const cJSON* cd = cJSON_GetObjectItem(it, "code");
                        if (cJSON_IsString(mkt) && cJSON_IsString(cd)) {
                            std::string m = mkt->valuestring;
                            /* 服务端返回的 market 可能带引号，兼容一下 */
                            m.erase(std::remove(m.begin(), m.end(), '"'), m.end());
                            first_code = m + cd->valuestring;
                        }
                    }
                    cJSON_Delete(root);
                }
                if (first_code.empty() || !api.StockQuote(first_code, &json)) {
                    return std::string("查询失败：没有找到这只股票，请确认代码或名称。");
                }
            }
            return FormatQuote(json);
        });

    mcp.AddTool(
        "self.stock.search",
        "按名称或代码搜索股票，返回候选列表（市场+代码+名称）。用于不确定代码时先查代码。",
        PropertyList({Property("keyword", kPropertyTypeString)}),
        [&api](const PropertyList& props) -> ReturnValue {
            std::string json;
            if (!api.StockSearch(props["keyword"].value<std::string>(), &json)) {
                return std::string("搜索失败：服务不可用。");
            }
            return FormatSearch(json);
        });

    ESP_LOGI(TAG, "stock tools registered");
}
