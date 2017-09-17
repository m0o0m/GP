#ifndef CoinConf_H
#define CoinConf_H

#include "BaseCoinConf.h"
#include "Table.h"

class CoinConf : public BaseCoinConf
{
public:
	static CoinConf* getInstance();
	virtual ~CoinConf();

	virtual bool GetCoinConfigData();

	Cfg* getCoinCfg() { return &coinCfg; }

	int getWinCount(int64_t &bankerwin, int64_t &userbanker); 
	int setWinCount(int64_t bankerwin, int64_t userbanker);
	int setTableBanker(int id);
	int setTableStatus(short status);
	int setPlayerBet(short type, int64_t betcoin);
	int clearPlayerUidBet();
	int setPlayerUidBet(short type, int uid, int64_t betcoin);
	int getSwtichTypeResult();
	int getBlackList(vector<int> &blacklist);

private:
	CoinConf();

	Cfg coinCfg;
};

#endif

