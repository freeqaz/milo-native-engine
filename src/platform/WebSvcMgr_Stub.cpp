#include "net/WebSvcMgr.h"

class WebSvcMgrStub : public WebSvcMgr {
public:
    bool DoRequest(ReqType, unsigned int, unsigned short, const char *, const char *,
                   unsigned int, const char *, unsigned int) override { return false; }
    bool InitRequest(WebSvcRequest *, ReqType, const char *, unsigned short,
                     const char *, unsigned int) override { return false; }
    bool InitRequest(WebSvcRequest *, ReqType, unsigned int, unsigned short,
                     const char *, unsigned int) override { return false; }
};

static WebSvcMgrStub gWebSvcMgr;
WebSvcMgr &TheWebSvcMgr = gWebSvcMgr;
