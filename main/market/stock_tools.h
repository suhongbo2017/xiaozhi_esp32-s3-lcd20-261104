#ifndef STOCK_TOOLS_H
#define STOCK_TOOLS_H

/** 股票行情 MCP 工具（self.stock.quote / self.stock.search），
 *  数据来自自建 music_server.py 的 stock 接口。 */
class StockTools {
public:
    /** 注册 self.stock.quote / self.stock.search（幂等） */
    static void Initialize();
};

#endif  // STOCK_TOOLS_H
