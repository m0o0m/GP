#ifndef _BetRaseProc_H_
#define _BetRaseProc_H_

#include "BaseProcess.h"

class Table;
class Player;

class BetRaseProc :public BaseProcess
{
	public:
		BetRaseProc();
		virtual ~BetRaseProc();
		virtual int doRequest(CDLSocketHandler* clientHandler, InputPacket* inputPacket,Context* pt )  ;
	private:
		int sendRaseInfoToPlayers(Player* player, Table* table, Player* betcallplayer,int64_t betmoney,Player* nextplayer,short seq);
		int PushCardToUser(Table* table);
		int sendCardInfo(Table* table,Player* player);
};

#endif

