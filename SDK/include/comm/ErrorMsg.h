#ifndef _ErrorMsg_
#define _ErrorMsg_

class RedisAccess;

class ErrorMsg
{
	public:
		static ErrorMsg* getInstance();
		const char* getErrMsg(int code);
	private:
		ErrorMsg();
		virtual ~ErrorMsg();
	public:
		void init(const char* file = "");
		void initFromBuf(const char *pErrMsg);
		bool initFromRedis(RedisAccess &redis, const char* gametag);

	private:
		bool binit;
};

#define _EMSG_(code) ErrorMsg::getInstance()->getErrMsg(code)
#define ERRMSG(code) ErrorMsg::getInstance()->getErrMsg(code)

#endif

