#include <string>
#include "ThrowCardProc.h"
#include "HallManager.h"
#include "Logger.h"
#include "Room.h"
#include "Configure.h"
#include "ErrorMsg.h"
#include "GameCmd.h"
#include "ProcessManager.h"
#include "BaseClientHandler.h"
#include "json/json.h"
#include "IProcess.h"

using namespace std;


ThrowCardProc::ThrowCardProc()
{
	this->name = "ThrowCardProc" ;
}

ThrowCardProc::~ThrowCardProc()
{
}

int ThrowCardProc::doRequest(CDLSocketHandler* clientHandler, InputPacket* pPacket,Context* pt)
{
	//_NOTUSED(pt);
	int cmd = pPacket->GetCmdType();
	short seq = pPacket->GetSeqNum();
	int uid = pPacket->ReadInt();
	int tid = pPacket->ReadInt();
	short svid = tid >> 16;
	short realTid = tid & 0x0000FFFF;
	
	_LOG_INFO_("==>[ThrowCardProc] [0x%04x] uid[%d]\n", cmd, uid);
	_LOG_DEBUG_("[DATA Parse] tid=[%d] svid=[%d] reallTid[%d]\n", tid, svid, realTid);

	BaseClientHandler* hallhandler = reinterpret_cast<BaseClientHandler*> (clientHandler);

	Room* room = Room::getInstance();

	Table* table = room->getTable(realTid);

	if(table == NULL)
	{
		_LOG_ERROR_("[ThrowCardProc] uid=[%d] tid=[%d] realTid=[%d] Table is NULL\n",uid, tid, realTid);
		return sendErrorMsg(hallhandler, cmd, uid, -2,ErrorMsg::getInstance()->getErrMsg(-2),seq);
	}

	Player* player = table->getPlayer(uid);
	if(player == NULL)
	{
		_LOG_ERROR_("[ThrowCardProc] uid=[%d] tid=[%d] realTid=[%d] Your not in This Table\n",uid, tid, realTid);
		return sendErrorMsg(hallhandler, cmd, uid, -1,ErrorMsg::getInstance()->getErrMsg(-1),seq);
	}

	if( ! player->isActive())
	{
		_LOG_ERROR_("[ThrowCardProc] uid=[%d] tid=[%d] realTid=[%d] tm_nStatus=[%d] is not active\n",
			uid, tid, realTid, table->m_nStatus);
		return sendErrorMsg(hallhandler, cmd, uid, -11,ErrorMsg::getInstance()->getErrMsg(-11),seq);
	}

	//不是私人场才这样处理
	if(table->m_nType != PRIVATE_ROOM)
	{
		if(table->m_nCurrBetUid != uid)
		{
			_LOG_ERROR_("[ThrowCardProc] uid=[%d] tid=[%d] realTid=[%d] betUid[%d] is not this uid\n",uid, tid, realTid, table->m_nCurrBetUid);
			return sendErrorMsg(hallhandler, cmd, uid, -12,ErrorMsg::getInstance()->getErrMsg(-12),seq);
		}
	}

	if(table->m_bIsOverTimer)
	{
		_LOG_ERROR_("[ThrowCardProc] table is IsOverTimer uid=[%d] tid=[%d] realTid=[%d] Table Status=[%d]\n",uid, tid, realTid, table->m_nStatus);
		return sendErrorMsg(hallhandler, cmd, uid, 2,ErrorMsg::getInstance()->getErrMsg(2),seq);
	}
	int64_t leaverSubCoin = player->betCoinList[0];
	//把离开的用户的信息记录下来，方便这局结算的时候告诉其它用户
	table->leaverarry[table->leavercount].uid = uid;
	table->leaverarry[table->leavercount].m_nTabIndex = player->m_nTabIndex;
	strcpy(table->leaverarry[table->leavercount].name, player->name);
	table->leaverarry[table->leavercount].betcoin = leaverSubCoin;
	table->leaverarry[table->leavercount].source = player->source;
	memcpy(table->leaverarry[table->leavercount].betCoinList, player->betCoinList, sizeof(int64_t)*5);
	table->leaverarry[table->leavercount].pid = player->pid;
	table->leaverarry[table->leavercount].sid = player->sid;
	table->leaverarry[table->leavercount].cid = player->cid;
	table->leaverarry[table->leavercount].m_bAllin = player->m_bAllin;
	table->leaverarry[table->leavercount].m_lMoney = player->m_lMoney - leaverSubCoin;
	table->leaverarry[table->leavercount].m_nWin = player->m_nWin;
	table->leaverarry[table->leavercount].m_nLose = player->m_nLose;
	table->leaverarry[table->leavercount].m_nRunAway = player->m_nRunAway;
	table->leaverarry[table->leavercount].m_lMaxWinMoney = player->m_lMaxWinMoney;
	table->leaverarry[table->leavercount].m_lMaxCardValue = player->m_lMaxCardValue;
	table->leaverarry[table->leavercount].m_nMagicNum = player->m_nMagicNum;
	table->leaverarry[table->leavercount].m_nMagicCoinCount = player->m_nMagicCoinCount;
	table->setSumPool();

	IProcess::updateDB_UserCoin(&table->leaverarry[table->leavercount], table);
	table->leavercount++;
	player->m_bHasCard = false;
	player->m_nStatus = STATUS_PLAYER_OVER;

	_LOG_INFO_("[ThrowCardProc] Uid=[%d] GameID=[%s] BetCountMoney=[%ld] currRound=[%d]\n", player->id, table->getGameID(), player->betCoinList[0], table->m_bCurrRound);
	Json::Value data;
	data["BID"] = string(table->getGameID());
	data["Time"]=(int)(time(NULL) - table->getStartTime());
	data["currd"] = table->m_bCurrRound;
	data["throwID"] = player->id;
	data["count"] = (double)player->betCoinList[0];
	if(!table->isAllRobot())
		_LOG_REPORT_(player->id, RECORD_THROW_CARD, "%s", data.toStyledString().c_str());
	data["errcode"] = 0;
	_UDP_REPORT_(player->id, cmd,"%s",data.toStyledString().c_str());

	//设置下一个应该下注的用户
	Player* nextplayer = table->getNextBetPlayer(player, OP_THROW);

	if(table->iscanGameOver())
		nextplayer = NULL;

	if(nextplayer)
	{
		table->setPlayerlimitcoin();
		table->setPlayerOptype(player, OP_THROW);
	}

	player->setActiveTime(time(NULL));

	PlayerSet::iterator it = table->m_Viewers.begin();
	for(int i = 0; i < GAME_PLAYER + (int)table->m_Viewers.size(); ++i)
	{
		Player *otherplayer = NULL;
		if (i < GAME_PLAYER)
			otherplayer = table->player_array[i];
		else {
			otherplayer = *it;
			it++;
		}
		if(otherplayer)
		{
			sendThrowInfoToPlayers(otherplayer, table, player, nextplayer,seq);
		}
	}
	if(table->iscanGameOver())
		return table->gameOver();
	//当前已经没有下一个用户下注了,此轮结束
	if(nextplayer == NULL)
	{
		table->setNextRound();	
	}
	else
	{
		table->stopBetCoinTimer();
		table->startBetCoinTimer(nextplayer->id,Configure::getInstance()->betcointime);
		nextplayer->setBetCoinTime(time(NULL));
	}

	return 0;
}


int ThrowCardProc::sendThrowInfoToPlayers(Player* player, Table* table, Player* throwcallplayer,Player* nextplayer, short seq)
{
	int svid = Configure::getInstance()->server_id;
	int tid = (svid << 16)|table->id;
	OutputPacket response;
	response.Begin(CLIENT_MSG_THROW_CARD, player->id);
	if(player->id == throwcallplayer->id)
		response.SetSeqNum(seq);
	response.WriteShort(0);
	response.WriteString("");
	response.WriteInt(player->id);
	response.WriteShort(player->m_nStatus);
	response.WriteInt(tid);
	response.WriteShort(table->m_nStatus);
	response.WriteByte(table->m_bCurrRound);
	response.WriteInt(throwcallplayer->id);
	response.WriteInt64(table->m_lCurrMax);
	response.WriteInt(nextplayer ? nextplayer->id : 0);
	response.WriteInt64(player->betCoinList[table->m_bCurrRound]);
	response.WriteInt64(player->m_lCarryMoney);
	response.WriteShort(player->optype);
	response.WriteInt64(player->m_lBetLimit);
	response.WriteInt64(table->getSumPool());
	response.End();
	_LOG_DEBUG_("<==[ThrowCardProc] Push [0x%04x] to uid=[%d]\n", CLIENT_MSG_THROW_CARD, player->id);
	_LOG_DEBUG_("[Data Response] err=[0], errmsg[]\n");
	_LOG_DEBUG_("[Data Response] uid=[%d]\n",player->id);
	_LOG_DEBUG_("[Data Response] m_nStatus=[%d]\n",player->m_nStatus);
	_LOG_DEBUG_("[Data Response] m_lBetLimit=[%d]\n", player->m_lBetLimit);
	_LOG_DEBUG_("[Data Response] optype=[%d]\n", player->optype);
	_LOG_DEBUG_("[Data Response] throwcallplayer=[%d]\n", throwcallplayer->id);
	_LOG_DEBUG_("[Data Response] m_bCurrRound=[%d]\n", table->m_bCurrRound);
	_LOG_DEBUG_("[Data Response] nextplayer=[%d]\n", nextplayer ? nextplayer->id : 0);

	if(HallManager::getInstance()->sendToHall(player->m_nHallid, &response, false) < 0)
		_LOG_ERROR_("[BetCallProc] Send To Uid[%d] Error!\n", player->id);
	return 0;
}

int ThrowCardProc::doResponse(CDLSocketHandler* clientHandler, InputPacket* inputPacket ,Context* pt )  
{
	_NOTUSED(clientHandler);
	_NOTUSED(inputPacket);
	_NOTUSED(pt);
	return 0;
}

REGISTER_PROCESS(CLIENT_MSG_THROW_CARD, ThrowCardProc)