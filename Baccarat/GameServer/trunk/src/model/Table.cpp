#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <Game/GameUtil.h>
#include "Table.h"
#include "Logger.h"
#include "IProcess.h"
#include "Configure.h"
#include "AllocSvrConnect.h"
#include "CoinConf.h"
#include "HallManager.h"
#include "Util.h"
#include "Protocol.h"
#include "GameApp.h"
#include "StrFunc.h"
#include "RedisLogic.h"

//区域倍率
BYTE Multiple[AREA_MAX] = {
    0 ,
    MULTIPLE_XIAN_PING ,
    MULTIPLE_PING ,
    MULTIPLE_ZHUANG_PING ,
    MULTIPLE_XIAN ,
    MULTIPLE_ZHUANG
};

// 庄赢 ，庄赢庄对，庄赢闲对, 庄赢庄对闲对，闲赢，闲赢闲对， 闲赢庄对，闲赢庄对闲对，平，平庄对，平闲对，平庄对闲对
BYTE AllWinArea[12][AREA_MAX] = {
    { FALSE , FALSE , FALSE , FALSE , FALSE , TRUE } , // 庄赢
    { FALSE , FALSE , FALSE , TRUE , FALSE , TRUE } , // 庄赢庄对
    { FALSE , TRUE , FALSE , FALSE , FALSE , TRUE } , // 庄赢闲对
    { FALSE , TRUE , FALSE , TRUE , FALSE , TRUE } , // 庄赢庄对闲对
    { FALSE , FALSE , FALSE , FALSE , TRUE , FALSE } , // 闲赢
    { FALSE , TRUE , FALSE , FALSE , TRUE , FALSE } , // 闲赢闲对
    { FALSE , FALSE , FALSE , TRUE , TRUE , FALSE } , // 闲赢庄对
    { FALSE , TRUE , FALSE , TRUE , TRUE , FALSE } , // 闲赢庄对闲对
    { FALSE , FALSE , TRUE , FALSE , FALSE , FALSE } , // 平
    { FALSE , FALSE , TRUE , TRUE , FALSE , FALSE } , // 平庄对
    { FALSE , TRUE , TRUE , FALSE , FALSE , FALSE } , // 平闲对
    { FALSE , TRUE , TRUE , TRUE , FALSE , FALSE }  // 平庄对闲对
};

// 查找指定上庄玩家的仿函数
class BankerFindFunctor
{
public:
	BankerFindFunctor(const int _uid) : m_uid(_uid) {}

	bool operator() (const std::list<Player *>::value_type &value)
	{
		return m_uid == value->id;
	}

private:
	int m_uid;
};

static bool compare_nocase (Player* first, Player* second)
{
	return (first)->m_lMoney > (second)->m_lMoney;
}

// 有概率要确保机器人庄家赢
static bool isMakeRobotBankerWin(Table *pTable)
{
    Player * pBanker = (pTable != NULL) ? pTable->getBanker() : NULL;
    if (pBanker == NULL || pBanker->source != E_MSG_SOURCE_ROBOT)
    {
        return false;
    }

    int random_result = Util::rand_range(0, 100);   // [0, 99]
    LOGGER(E_LOG_INFO) << "random_result = " << random_result;

    return (random_result < Configure::getInstance()->robotBankerWinRate) ? true : false;
}

Cfg Table::coincfg;

static std::string game_name = "Baccarat";

Table::Table():
    m_nStatus(-1), 
    receivePhpPush(false), 
    zhuangDui(0), 
    xianDui(0),
    m_nZhuangWinRate(458597) ,
    m_nXianWinRate(446247) ,
    m_nPingRate(95156) ,
    m_nZhuangDuiRate(50400) ,
    m_nXianDuiRate(50400),
    m_RemainCardCount(CGameLogic::GetCardCount()),
    m_NeedMakeRobotBankerWin(false)
{
	m_robotbanker.init();
	m_robotbanker.m_lMoney = ROBOT_BANKER_MONEY;
	m_robotbanker.id = ROBOT_BANKER_ID;
	m_robotbanker.source = E_MSG_SOURCE_ROBOT;

	reloadCfg();
}

Table::~Table()
{
}

void Table::init()
{
	timer.init(this);
	this->m_nStatus = STATUS_TABLE_EMPTY;
	memset(player_array, 0, sizeof(player_array));
	BankerList.clear();
	memset(m_lTabBetArray, 0, sizeof(m_lTabBetArray));
    memset(m_lRealBetArray, 0, sizeof(m_lRealBetArray));
    memset(betAreaLimit, 0, sizeof(betAreaLimit));
    memset(m_nChipArray, 0, sizeof(m_nChipArray));
	m_bHasBet = false;

	maxwinner = NULL;
	m_lBankerWinScoreCount = 0;
	//this->startIdleTimer(Configure::getInstance()->idletime);
	//this->setStatusTime(time(NULL));
	m_nRecordLast=0;
	memset(m_GameRecordArrary, 0, sizeof(m_GameRecordArrary));
	bankeruid = 0;
	bankernum = 0;
	m_nLimitCoin = 0;
	this->m_nPersonLimitCoin = 100000;
	betVector.clear();
	reloadCfg();
	m_nDespatchNum = 0;

	for (int i = 0; i < MAX_SEAT_NUM; ++i)
    {
        m_SeatidArray[i] = NULL;
    }
    memset(m_cbCardCount, 0, sizeof(m_cbCardCount));
    memset(m_cbTableCardArray, 0, sizeof(m_cbTableCardArray));
    m_cbBankerPoint = 0;
    m_cbPlayerPoint = 0;
    m_cbBankerPair = 0;
    m_cbPlayerPair = 0;
    m_historyCount = 0;

	receivePhpPush = false;
	//机器人坐庄
	m_robotbanker.m_lMoney = ROBOT_BANKER_MONEY; //每局开始机器人钱重置，避免机器人连续坐庄可能的钱不够的问题
	setToBankerList(&m_robotbanker);
	playerComming(&m_robotbanker);	// 机器人先把0号位置占掉
	rotateBanker();
}

void Table::reset()
{
	memset(m_lTabBetArray, 0, sizeof(m_lTabBetArray));
	memset(m_lRealBetArray, 0, sizeof(m_lRealBetArray));
	m_bHasBet = false;
	m_lBankerWinScoreCount = 0;
	for(int i = 0; i < GAME_PLAYER; ++i)
	{
		Player* player = player_array[i];
		if(player)
			player->reset();
	}
	betVector.clear();
	maxwinner = NULL;
	Player* banker = NULL;
	banker = this->getBanker();
	if(banker)
	{
        calculateBetAreaLimit(banker);
	}
	m_nDespatchNum = 0;
    m_cbBankerPoint = 0;
    m_cbPlayerPoint = 0;
    m_cbBankerPair = 0;
    m_cbPlayerPair = 0;

	memset(m_cbCardCount, 0, sizeof(m_cbCardCount));
	memset(m_cbTableCardArray, 0, sizeof(m_cbTableCardArray));
	receivePhpPush = false;

    if (m_RemainCardCount <= SHUFFLE_CRITICAL_COUNT)
    {
        m_RemainCardCount = CGameLogic::GetCardCount();
    }
}

void Table::setGameID(char* gameID, Player* banker /*= NULL*/)
{
	memset(GameID, 0, sizeof(GameID));

	if (gameID == NULL)
	{
		time_t t;
		time(&t);
		char time_str[32] = { 0 };
		struct tm* tp = localtime(&t);
		strftime(time_str, 32, "%Y%m%d%H%M%S", tp);
		if (banker == NULL)
		{
			banker = this->getBanker();
		}

		sprintf(GameID, "%s|%d|%02d", time_str, banker ? banker->id : 0, this->bankernum + 1);
	}
	else
	{
		strncpy(GameID, gameID, sizeof(GameID) - 1);
	}
}

void Table::setStartTime(time_t t)
{
	StartTime = t;
	bool bRet = Server()->HSETi(StrFormatA("%s:%d", game_name.c_str(), Configure::getInstance()->m_nServerId).c_str(),
		"starttime", t);
	if (!bRet)
	{
		LOGGER(E_LOG_ERROR) << "save time to redis failed!";
	}
}

Player* Table::isUserInTab(int uid)
{
	for(int i = 0; i < GAME_PLAYER; ++i)
	{
		Player* player = player_array[i];
		if(player && player->id == uid)
			return player;
	}
	return NULL;
}

bool Table::isAllRobot()
{
	for(int i = 0; i < GAME_PLAYER; ++i)
	{
		if(player_array[i] && player_array[i]->source != 30 && player_array[i]->m_lBetArray[0] > 0)
		{
			return false;
		}
	}
	return true;
}

Player* Table::getPlayer(int uid)
{
	for(int i = 0; i < GAME_PLAYER; ++i)
	{
		Player* player = player_array[i];
		if(player && player->id == uid)
			return player;
	}
    return NULL;
}

// 请求上庄
void Table::setToBankerList(Player* player)
{
	if (player == NULL)
	{
		return;
	}

	if (isWaitForBanker(player->id))
	{
		ULOGGER(E_LOG_INFO, player->id) << "Player is in banker list already!";
		return;
	}

	BankerList.push_back(player); BankerList.front();
}

// 请求下庄
void Table::CancleBanker(Player* player)
{
	if (player == NULL)
	{
		return;
	}

	if (!isWaitForBanker(player->id))
	{
		ULOGGER(E_LOG_INFO, player->id) << "Player isn't in banker list!";
		return;
	}

	list<Player*>::iterator it = BankerList.begin();
	while(it != BankerList.end())
	{
		Player* noer = *it;
		if(noer != NULL && noer->id == player->id)
		{
			BankerList.erase(it++);
		}
		else
		{
			it++;
		}
	}
}

// 自动移出上庄请求列表
void Table::HandleBankeList()
{
	list<Player*>::iterator it;
	it = BankerList.begin();
	while(it != BankerList.end())
	{
		Player* other = *it;
		if(other)
		{
			if(other->m_lMoney < m_nBankerlimit)
			{
				IProcess::NoteBankerLeave(this, other);
				BankerList.erase(it++);
			}
			else
				it++;
		}
		else
			BankerList.erase(it++);
	}
}


void Table::rotateBanker(Player* specified_player /*= NULL*/)
{
	// s1. 选出新的庄家
	Player *banker = specified_player;			// P1
	if (banker == NULL)							// P2
	{
		if (BankerList.size() == 0)
		{
			banker = &m_robotbanker;
		}
		else
		{
			list<Player *>::iterator it = BankerList.begin();
			while (it != BankerList.end())
			{
				banker = *it;
				if (banker == NULL)
				{
					continue;
				}

				if (!banker->isonline)
				{
					BankerList.erase(it++);
					continue;
				}

				break;
			}
		}
	}

	if (banker != NULL)
	{
		// remove from banker list.
		if ( isWaitForBanker(banker->id) )
		{
			BankerList.remove(banker);
		}
	}
	else
	{
		banker = &m_robotbanker;			// P3
	}

	assert(banker != NULL);

	// s2. 是否连续做庄 (只允许系统连庄)
	LOGGER(E_LOG_INFO) << "cur_banker_id = " << bankeruid << ", new_banker_id = " << banker->id;
	if (banker->id == bankeruid && bankeruid == ROBOT_BANKER_ID)
	{
		ULOGGER(E_LOG_INFO, bankeruid) << "continue banking...";
		return;
	}

	// s3. 换庄 (需要换庄; 换庄只是打个标记, 座位号并未发生变化)
	bankernum = 0;
	Player* prebanker = getPlayer(bankeruid);

	setBetLimit();
	setGameID(NULL, banker);

	// down
	if (prebanker != NULL)
	{
		prebanker->m_nStatus = (prebanker->id == m_robotbanker.id) ? STATUS_PLAYER_LOGOUT : STATUS_PLAYER_RSTART;
		ULOGGER(E_LOG_INFO, prebanker->id) << "==> banker -> player";
	}

	// up
	bankeruid = banker->id;//做庄的uid
	banker->m_nStatus = STATUS_PLAYER_BANKING;

    // 系统做庄时, 如果中途有玩家申请上庄, 刚下局游戏马上进行庄家轮换
    if (banker->source == E_MSG_SOURCE_ROBOT)
    {
        bankernum = m_nMaxBankerNum;
    }

	ULOGGER(E_LOG_INFO, banker->id) << "==> player -> banker, bankernum = " << bankernum;

	// send notification
	this->NotifyPlayerSeatInfo();
	IProcess::rotateBankerInfo(this, banker, prebanker);
}

int Table::playerComming(Player* player)
{
	if(player == NULL)
		return -1;

	if (!isUserInTab(player->id))
	{
		//说明此房间这个用户进入不成功，返回-2之后重新给此用户分配房间
		int nResult = playerSeatInTab(player);
		ULOGGER(E_LOG_INFO, player->id) << "sit result = " << nResult;
		if (nResult < 0)
		{
			return -2;
		}
	}
	else
	{
		player->enter();
		return 0;
	}

	if (!this->isActive())
		this->m_nStatus = STATUS_TABLE_READY;

	player->tid = this->id;
	player->enter();
	ULOGGER(E_LOG_INFO, player->id) << "player add";
	AllocSvrConnect::getInstance()->updateTableUserCount(this);
	return 0;
}

int Table::playerSeatInTab(Player* complayer)
{
	for (int i = 0; i < GAME_PLAYER; i++)
	{
		if (player_array[i] == NULL)
		{
			player_array[i] = complayer;
			complayer->m_nStatus = STATUS_PLAYER_COMING;
			complayer->tid = this->id;
			complayer->m_nSeatID = i;
			ULOGGER(E_LOG_INFO, complayer->id) << "tid = " << this->id << ", m_nSeatID = " << complayer->m_nSeatID;
			return 0;
		}
	}

	return -1;
}

int Table::playerStandUp(Player* player)
{
	if (player->m_nSeatID == -1)
		return 0;

	if (isUserInTab(player->id)) {
		player->standup();
		//setSeatNULL(player);
		//if (player->id != bankeruid)
		//	JoinViewers(player);
	}

	return 0;
}

int Table::playerSeatDown(Player* player, int tabindex, bool isChangeSeat /*= false*/)
{
	// 座位号错误
	if (tabindex < 0 || tabindex >= GAME_PLAYER)
		return -38;

	// 判断当前座位是否已经有玩家
	Player *pSeatPlayer = NULL;
	for (int idx = 0; idx < GAME_PLAYER; ++idx)
	{
		Player *p = player_array[idx];	// 数组索引 ！= 座位号
		if (p == NULL)
		{
			continue;
		}

		if (p->m_nSeatID == tabindex)
		{
			pSeatPlayer = p;
			break;
		}
	}
	if (pSeatPlayer != NULL && pSeatPlayer->id != player->id)
	{
		LOGGER(E_LOG_ERROR) << "SeatPlayer_id = " << pSeatPlayer->id << " player_id = " << player->id;
		return -37;
	}

	if (isUserInTab(player->id)) 
	{
		for (int i = 0; i < GAME_PLAYER; ++i) 
		{
			Player *current = player_array[i];
			if (current == NULL || current->id != player->id)
			{
				continue;
			}
			
			if (isChangeSeat)
			{
				// 换座
				if (tabindex != i)
				{
					playerStandUp(current);
					break;
				}
			}
			else
			{
				// 入座
				current->sitdown();
			}
		}
	}

	return playerComming(player);
}

int Table::playerChangeSeat(Player* player, int tabindex)
{
	if (tabindex < 0 || tabindex >= GAME_PLAYER)
		return -38;

	if (player_array[tabindex] != NULL && player_array[tabindex]->id != player->id) {
		return -37;
	}

	playerStandUp(player);
	return playerSeatDown(player, tabindex, true);
}

void Table::setSeatNULL(Player* player)
{
	for(int i = 0; i < GAME_PLAYER; ++i)
	{
		if(player_array[i] == player)
			player_array[i] = NULL;
	}

	if (player->source == E_MSG_SOURCE_ROBOT)
	{
		return;
	}

	if(isWaitForBanker(player->id))
	{
		list<Player*>::iterator it = BankerList.begin();
		while(it != BankerList.end())
		{
			Player* other = *it;
			if(other)
			{
				if(other->id == player->id)
				{
					BankerList.erase(it);
					break ;
				}
			}
			it++;
		}
	}
}

void Table::playerLeave(int uid)
{
	Player*  player = this->getPlayer(uid);
	if(player)
	{
		this->playerLeave(player);
	}
}

void Table::playerLeave(Player* player)
{
	if(player == NULL)
		return;
	//_LOG_WARN_("Player[%d] Leave\n", player->id);
	ULOGGER(E_LOG_INFO, player->id) << "leave";
	
	this->setSeatNULL(player);
	
	if (player->m_nSeatID != 0)
    {
        this->LeaveSeat(player->m_nSeatID);
        this->NotifyPlayerSeatInfo();
    }
	
	player->leave();
	player->init();
	//如果桌子不是在玩牌则状态变为等待准备状态
	if (!this->isActive())
	{
		this->m_nStatus = STATUS_TABLE_READY;
	}
	ULOGGER(E_LOG_INFO, player->id) << "player leave";

	if (this->isEmpty())
	{
		this->init();
	}
	
	AllocSvrConnect::getInstance()->updateTableUserCount(this);
}

int Table::playerBetCoin(Player* player, short bettype, int64_t betmoney)
{
	if(player == NULL)
		return -3;

	if(bettype <= 0 || bettype > 5)
		return -3;
    
    if(player->m_lBetArray[bettype] > this->betAreaLimit[bettype])
    {
        _LOG_ERROR_("You[%d] is large limit[%ld] bet type[%d] coin[%ld]\n", player->id, this->betAreaLimit[bettype], bettype, betmoney);
        return -15;
    }
    
    int64_t total_bet = 0;
    for (int i = 1; i < BETNUM; i++)
    {
        total_bet += player->m_lBetArray[i];
    }
	if ((total_bet + betmoney) > m_nMaxBetNum)
	{
		_LOG_ERROR_("You[%d] is large sum limit[%ld] bet type[%d] coin[%ld]\n", player->id, m_nMaxBetNum, bettype, player->m_lBetArray[bettype] + betmoney);
		return -8;
	}

	if(player->m_lMoney < betmoney)
	{
		_LOG_ERROR_("You[%d] not Not enough money[%ld] bet type[%d] coin[%ld]\n", player->id, player->m_lMoney, bettype, betmoney);
		return -13;
	}

	player->m_lMoney -= betmoney;
	player->m_lBetArray[bettype] += betmoney;
	player->m_lBetArray[0] += betmoney;
	ULOGGER(E_LOG_INFO, player->id) << "bet array[0] = " << player->m_lBetArray[0];
	this->m_lTabBetArray[bettype] += betmoney;
	this->m_lTabBetArray[0] += betmoney;
	//if (bettype == AREA_XIAN || bettype == AREA_ZHUANG)
	//{
		player->m_lReturnScore += betmoney;
	//}
	player->m_nLastBetTime = time(NULL);
	if(player->source != E_MSG_SOURCE_ROBOT)
	{
		this->m_lRealBetArray[bettype] += betmoney;
		SaveBetInfoToRedis(player, bettype);
		m_bHasBet = true;
	}
	setBetLimit();
	return 0;
}

void Table::calculateBetAreaLimit(Player* banker)
{
    betAreaLimit[AREA_XIAN_DUI] = (banker->m_lMoney + m_lTabBetArray[AREA_PING] + m_lTabBetArray[AREA_ZHUANG] + m_lTabBetArray[AREA_ZHUANG_DUI] - m_lTabBetArray[AREA_XIAN]) / 11;
    betAreaLimit[AREA_PING] = (banker->m_lMoney + m_lTabBetArray[AREA_XIAN_DUI] + m_lTabBetArray[AREA_XIAN] + m_lTabBetArray[AREA_ZHUANG] + m_lTabBetArray[AREA_ZHUANG_DUI]) / 9;
    betAreaLimit[AREA_ZHUANG_DUI] = (banker->m_lMoney + m_lTabBetArray[AREA_PING] + m_lTabBetArray[AREA_XIAN_DUI] + m_lTabBetArray[AREA_XIAN] - m_lTabBetArray[AREA_ZHUANG]) / 11;
    betAreaLimit[AREA_XIAN] = banker->m_lMoney + m_lTabBetArray[AREA_PING] + m_lTabBetArray[AREA_ZHUANG] + m_lTabBetArray[AREA_ZHUANG_DUI] - m_lTabBetArray[AREA_XIAN_DUI] * 11;
    betAreaLimit[AREA_ZHUANG] = banker->m_lMoney + m_lTabBetArray[AREA_PING] + m_lTabBetArray[AREA_XIAN] + m_lTabBetArray[AREA_XIAN_DUI] - m_lTabBetArray[AREA_ZHUANG_DUI] * 11;
    LOGGER(E_LOG_INFO) << "xian dui limit = " << betAreaLimit[AREA_XIAN_DUI]
                       << "he limit = " << betAreaLimit[AREA_PING]
                       << "zhuang dui limit = " << betAreaLimit[AREA_ZHUANG_DUI]
                       << "xian limit = " << betAreaLimit[AREA_XIAN]
                       << "xian dui limit = " << betAreaLimit[AREA_ZHUANG];
}

BYTE Table::getWinType(BYTE cbWinArea[AREA_MAX])
{
    if (cbWinArea[AREA_ZHUANG] == TRUE /*|| cbWinArea[AREA_ZHUANG_DUI] == TRUE*/)
    {
        return WIN_TYPE_ZHUANG;
    }
    if (cbWinArea[AREA_XIAN] == TRUE /*|| cbWinArea[AREA_XIAN_DUI] == TRUE*/)
    {
        return WIN_TYPE_XIAN;
    }
    if (cbWinArea[AREA_PING] == TRUE)
    {
        return WIN_TYPE_HE;
    }
    LOGGER(E_LOG_ERROR) << "win type error!";
    return WIN_TYPE_NONE;
}

short Table::getPlayerCount(bool canPlayOnly, bool exceptRobot /*= false*/) const
{
	short ret = 0;

	for (int idx = 0; idx < GAME_PLAYER; ++idx)
	{
		Player *current = player_array[idx];
		if (current == NULL)
		{
			continue;
		}

		if (current->source == E_MSG_SOURCE_ROBOT && exceptRobot)
		{
			continue;
		}

		if (!IProcess::CanPlay(current) && canPlayOnly)
		{
			continue;
		}

		++ret;
	}
	LOGGER(E_LOG_DEBUG) << "There are " << ret << " player in this table(id = " << id << ")";

	return ret;
}

bool Table::isWaitForBanker(int uid) const
{
	std::list<Player *>::const_iterator dest = std::find_if(BankerList.begin(), BankerList.end(), BankerFindFunctor(uid));
	return (dest != BankerList.end()) ? true : false;
}

void Table::receivePush(int winarea, BYTE zhuangdui, BYTE xiandui)
{
	receivePhpPush = true;
	winType = winarea;
	zhuangDui = zhuangdui;
	xianDui = xiandui;
}

BYTE find_dui_card(BYTE* cards, size_t count, BYTE card, CGameLogic* logic)
{
	for (size_t i = 0; i < count; i++)
	{
		if (logic->GetCardValue(card) == logic->GetCardValue(cards[i]))
		{
			return cards[i];
			break;
		}
	}
	return 0;
}

BYTE find_not_dui_card(BYTE* cards, size_t count, BYTE card, CGameLogic* logic)
{
	for (size_t i = 0; i < count; i++)
	{
		if (logic->GetCardValue(card) != logic->GetCardValue(cards[i]))
		{
			return cards[i];
			break;
		}
	}
	return 0;
}

BYTE find_card(BYTE* cards, size_t count, CGameLogic* logic, int randSeed)
{
    //srand(randSeed);
	size_t index = rand() % count;
	return cards[index];
}

// pip 点数， card_count 玩家最终手牌数量
void find_card(BYTE* cards, size_t all_count, CGameLogic* logic, BYTE* playerCards, BYTE dui, BYTE& pip, BYTE& card_count, int randSeed)
{
	//抽取第一张
	logic->RandCardList(playerCards, 1);
	//第二张
	if (dui == 1) //有对
	{
		playerCards[1] = find_dui_card(cards, all_count, playerCards[0], logic);
		if (playerCards[1] == 0) //如果实在找不到，直接使用第一张牌，不应该走到这里
		{
			playerCards[1] = playerCards[0];
		}
	}
	else
	{
		playerCards[1] = find_not_dui_card(cards, all_count, playerCards[0], logic);
	}
	pip = logic->GetCardListPip(playerCards, 2);
	card_count = 2;
	if (pip < 8) //补第三张
	{
		playerCards[2] = find_card(cards, all_count, logic, randSeed);
		pip = logic->GetCardListPip(playerCards, 3); //重新计算庄点
		card_count++;
	}
}

void Table::phpRandCard(int randSeed)
{
    srand(randSeed);

	if (!receivePhpPush)
	{
		LOGGER(E_LOG_WARNING) << "dont receive php push!";
		return;
	}
	static const int limitCount = 1000;
	BYTE card_array[2][3];
	size_t count = 0;
	BYTE* cards = m_GameLogic.Shuffle(count,rand());
	BYTE bankerPip, bankerCount;
	find_card(cards, count, &m_GameLogic, card_array[0], zhuangDui, bankerPip, bankerCount, rand());
	BYTE playerPip, playerCount;
	find_card(cards, count, &m_GameLogic, card_array[1], xianDui, playerPip, playerCount, rand());
	int index = 0;
	if (winType == WIN_TYPE_HE) //和赢
	{
		while (playerPip != bankerPip)
		{
			find_card(cards, count, &m_GameLogic, card_array[1], xianDui, playerPip, playerCount, rand());
			if (index >= limitCount)
			{
				LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
				m_GameLogic.RandCardList(m_cbTableCardArray[0], sizeof(m_cbTableCardArray) / sizeof(BYTE));
				return;
			}
			index++;
		}
	}
	else
	{
		if (winType == WIN_TYPE_ZHUANG) //庄赢
		{
			while (bankerPip <= 3) //点数太小，庄重新抽取
			{
				find_card(cards, count, &m_GameLogic, card_array[0], zhuangDui, bankerPip, bankerCount, rand());
				if (index >= limitCount)
				{
					LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
					break;
				}
				index++;
			}
			index = 0; // reset index
			while (playerPip >= 4) //点数太大，闲重新抽取
			{
				find_card(cards, count, &m_GameLogic, card_array[1], xianDui, playerPip, playerCount, rand());
				if (index >= limitCount)
				{
					LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
					m_GameLogic.RandCardList(m_cbTableCardArray[0], sizeof(m_cbTableCardArray) / sizeof(BYTE));
					return;
				}
				index++;
			}
			index = 0; // reset index
			while (playerPip >= bankerPip)
			{
				find_card(cards, count, &m_GameLogic, card_array[1], xianDui, playerPip, playerCount, rand());
				if (index >= limitCount)
				{
					LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
					m_GameLogic.RandCardList(m_cbTableCardArray[0], sizeof(m_cbTableCardArray) / sizeof(BYTE));
					return;
				}
				index++;
			}
		}
		else //闲赢
		{
			while (playerPip <= 3) //点数太小，闲重新抽取
			{
				find_card(cards, count, &m_GameLogic, card_array[1], xianDui, playerPip, playerCount, rand());
				if (index >= limitCount)
				{
					LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
					m_GameLogic.RandCardList(m_cbTableCardArray[0], sizeof(m_cbTableCardArray) / sizeof(BYTE));
					break;
				}
				index++;
			}
			index = 0; // reset index
			while (bankerPip >= 4) //点数太大，庄重新抽取
			{
				find_card(cards, count, &m_GameLogic, card_array[0], zhuangDui, bankerPip, bankerCount, rand());
				if (index >= limitCount)
				{
					LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
					m_GameLogic.RandCardList(m_cbTableCardArray[0], sizeof(m_cbTableCardArray) / sizeof(BYTE));
					return;
				}
				index++;
			}
			index = 0; // reset index
			while (bankerPip >= playerPip)
			{
				find_card(cards, count, &m_GameLogic, card_array[1], xianDui, playerPip, playerCount, rand());
				if (index >= limitCount)
				{
					LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
					m_GameLogic.RandCardList(m_cbTableCardArray[0], sizeof(m_cbTableCardArray) / sizeof(BYTE));
					return;
				}
				index++;
			}
		}
	}
	CopyMemory(m_cbTableCardArray[INDEX_BANKER], card_array[0], sizeof(card_array[0]));
	CopyMemory(m_cbTableCardArray[INDEX_PLAYER], card_array[1], sizeof(card_array[1]));
}


bool Table::hasSameCards(BYTE cards1[], BYTE count1, BYTE cards2[], BYTE count2)
{
    // 庄闲的前两张牌不能牌面重复
    if ((cards1[1] == cards2[1] && cards1[2] == cards2[2]) || (cards1[1] == cards2[2] && cards1[2] == cards2[1]))
    {
        return true;
    }

    return false;
}


BYTE Table::RandCardByWinType(int randSeed)
{
    srand(randSeed);

    int32_t win_rate = rand() % 1000000;
    _LOG_ERROR_("the rand win_rate is :%d", win_rate);
    BYTE type = 0;

    if (win_rate < m_nZhuangWinRate )
    {
        type = WIN_TYPE_ZHUANG;
    }
    else if (win_rate < m_nZhuangWinRate + m_nXianWinRate)
    {
        type = WIN_TYPE_XIAN;
    }
    else
    {
        type = WIN_TYPE_HE;
    }

    int32_t bank_dui_rate = rand() % 1000000;
    BYTE bank_dui = bank_dui_rate < m_nZhuangDuiRate? 1 : 0;
    _LOG_ERROR_("the rand bank_dui_rate is :%d", bank_dui_rate);

    int32_t xian_dui_rate = rand() % 1000000;
    BYTE xian_dui = xian_dui_rate < m_nXianDuiRate ? 1 : 0;
    _LOG_ERROR_("the rand xian_dui_rate is :%d", xian_dui_rate);

    static const int limitCount = 1000;
    BYTE card_array[2][3];
    size_t count = 0;
    BYTE* cards = m_GameLogic.Shuffle(count, rand());

    BYTE bankerPip , bankerCount;
    find_card(cards , count , &m_GameLogic , card_array[0] , bank_dui , bankerPip , bankerCount, rand());

    BYTE playerPip , playerCount;
    find_card(cards , count , &m_GameLogic , card_array[1] , xian_dui , playerPip , playerCount, rand());

    int index = 0;
    if (type == WIN_TYPE_HE) //和赢
    {
        while (playerPip != bankerPip || hasSameCards(card_array[0], bankerCount, card_array[1], playerCount))
        {
            find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
            if (index >= limitCount)
            {
                LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                return WIN_TYPE_NONE;
            }
            index++;
        }
    }
    else
    {
        if (type == WIN_TYPE_ZHUANG) //庄赢
        {
            while (bankerPip <= 3) //点数太小，庄重新抽取
            {
                find_card(cards , count , &m_GameLogic , card_array[0] , zhuangDui , bankerPip , bankerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    break;
                }
                index++;
            }
            index = 0; // reset index
            while (playerPip >= 4) //点数太大，闲重新抽取
            {
                find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    return WIN_TYPE_NONE;
                }
                index++;
            }
            index = 0; // reset index
            while (playerPip >= bankerPip || hasSameCards(card_array[0], bankerCount, card_array[1], playerCount))
            {
                find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    return WIN_TYPE_NONE;
                }
                index++;
            }
        }
        else //闲赢
        {
            while (playerPip <= 3) //点数太小，闲重新抽取
            {
                find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    break;
                }
                index++;
            }
            index = 0; // reset index
            while (bankerPip >= 4) //点数太大，庄重新抽取
            {
                find_card(cards , count , &m_GameLogic , card_array[0] , zhuangDui , bankerPip , bankerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    return WIN_TYPE_NONE;
                }
                index++;
            }
            index = 0; // reset index
            while (bankerPip >= playerPip || hasSameCards(card_array[0], bankerCount, card_array[1], playerCount))
            {
                find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    return WIN_TYPE_NONE;
                }
                index++;
            }
        }
    }
    CopyMemory(m_cbTableCardArray[INDEX_BANKER] , card_array[0] , sizeof(card_array[0]));
    CopyMemory(m_cbTableCardArray[INDEX_PLAYER] , card_array[1] , sizeof(card_array[1]));

    return type;
}


BYTE Table::RandCardByAssignedType(int randSeed, BYTE type)
{
    srand(randSeed);

    _LOG_ERROR_("the assigned type is :%d" , type);

    int32_t bank_dui_rate = rand() % 1000000;
    BYTE bank_dui = bank_dui_rate < m_nZhuangDuiRate ? 1 : 0;
    _LOG_ERROR_("the rand bank_dui_rate is :%d" , bank_dui_rate);

    int32_t xian_dui_rate = rand() % 1000000;
    BYTE xian_dui = xian_dui_rate < m_nXianDuiRate ? 1 : 0;
    _LOG_ERROR_("the rand xian_dui_rate is :%d" , xian_dui_rate);

    static const int limitCount = 1000;
    BYTE card_array[2][3];
    size_t count = 0;
    BYTE* cards = m_GameLogic.Shuffle(count, rand());

    BYTE bankerPip , bankerCount;
    find_card(cards , count , &m_GameLogic , card_array[0] , bank_dui , bankerPip , bankerCount, rand());

    BYTE playerPip , playerCount;
    find_card(cards , count , &m_GameLogic , card_array[1] , xian_dui , playerPip , playerCount, rand());

    int index = 0;
    if (type == WIN_TYPE_HE) //和赢
    {
        while (playerPip != bankerPip && hasSameCards(card_array[0], bankerCount, card_array[1], playerCount))
        {
            find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
            if (index >= limitCount)
            {
                LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                return WIN_TYPE_NONE;
            }
            index++;
        }
    }
    else
    {
        if (type == WIN_TYPE_ZHUANG) //庄赢
        {
            while (bankerPip <= 3) //点数太小，庄重新抽取
            {
                find_card(cards , count , &m_GameLogic , card_array[0] , zhuangDui , bankerPip , bankerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    break;
                }
                index++;
            }
            index = 0; // reset index
            while (playerPip >= 4) //点数太大，闲重新抽取
            {
                find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    return WIN_TYPE_NONE;
                }
                index++;
            }
            index = 0; // reset index
            while (playerPip >= bankerPip && hasSameCards(card_array[0], bankerCount, card_array[1], playerCount))
            {
                find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    return WIN_TYPE_NONE;
                }
                index++;
            }
        }
        else //闲赢
        {
            while (playerPip <= 3) //点数太小，闲重新抽取
            {
                find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    break;
                }
                index++;
            }
            index = 0; // reset index
            while (bankerPip >= 4) //点数太大，庄重新抽取
            {
                find_card(cards , count , &m_GameLogic , card_array[0] , zhuangDui , bankerPip , bankerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    return WIN_TYPE_NONE;
                }
                index++;
            }
            index = 0; // reset index
            while (bankerPip >= playerPip && hasSameCards(card_array[0], bankerCount, card_array[1], playerCount))
            {
                find_card(cards , count , &m_GameLogic , card_array[1] , xianDui , playerPip , playerCount, rand());
                if (index >= limitCount)
                {
                    LOGGER(E_LOG_WARNING) << "rand special card failed, user normal rand now";
                    m_GameLogic.RandCardList(m_cbTableCardArray[0] , sizeof(m_cbTableCardArray) / sizeof(BYTE));
                    return WIN_TYPE_NONE;
                }
                index++;
            }
        }
    }
    CopyMemory(m_cbTableCardArray[INDEX_BANKER] , card_array[0] , sizeof(card_array[0]));
    CopyMemory(m_cbTableCardArray[INDEX_PLAYER] , card_array[1] , sizeof(card_array[1]));

    return type;
}



void Table::SaveBetInfoToRedis(Player * player, int bettype)
{
	bool bRet = Server()->HSETi(StrFormatA("%s:%d:%d", game_name.c_str(),
		Configure::getInstance()->m_nServerId,
		bettype).c_str(),
		StrFormatA("%d", player->id).c_str(),
		player->m_lBetArray[bettype]);
	if (!bRet)
	{
		LOGGER(E_LOG_ERROR) << "save to redis failed!bettype = " << bettype << " betmoney = " << player->m_lBetArray[bettype];
	}
}

void Table::ClearBetInfo()
{
	for (int i = 1; i < BETNUM; i++)
	{
		bool bRet = Server()->DEL(StrFormatA("%s:%d:%d",
			game_name.c_str(),
			Configure::getInstance()->m_nServerId,
			i).c_str());
		if (!bRet)
		{
			LOGGER(E_LOG_ERROR) << "save to redis failed!bettype = " << i;
		}
	}
}

void Table::setBetLimit()
{
	Player* banker = NULL;
	banker = this->getBanker();
	if(banker)
	{
        calculateBetAreaLimit(banker);
	}
    else
    {
        LOGGER(E_LOG_ERROR) << "banker is INVALID!";
    }
}

bool Table::SetResult(int64_t bankerwincount, int64_t userbankerwincount)
{
    //计算牌点
    BYTE cbPlayerCount = m_GameLogic.GetCardListPip( m_cbTableCardArray[INDEX_PLAYER],m_cbCardCount[INDEX_PLAYER] );
    BYTE cbBankerCount = m_GameLogic.GetCardListPip( m_cbTableCardArray[INDEX_BANKER],m_cbCardCount[INDEX_BANKER] );
    //系统输赢
    int64_t lSystemScoreCount = 0;
    int64_t systemWinCount = 0;
    //推断玩家
    BYTE cbWinArea[AREA_MAX] = {FALSE};
    DeduceWinner(cbWinArea);
    
    Player* banker = NULL;
    banker = this->getBanker();
    
    if(banker == NULL)
    {
        //_LOG_ERROR_("CalculateScore: banker is NULL bankersid:%d bankeruid:%d\n", bankersid, bankeruid);
        return false;
    }

    for (int i = 0; i < GAME_PLAYER; i++)
    {
        Player* player = this->player_array[i];
        if (player)
        {
            player->m_lTempScore = 0;
        }
    }

    // 统计此牌型的输赢结果
    for(int j = 0; j < GAME_PLAYER; ++j)
    {
        Player *player = this->player_array[j];
        if (!IProcess::CanPlay(player))
            continue;

        if (player->id == this->bankeruid)
            continue;

        for (BYTE wAreaIndex = 1; wAreaIndex < AREA_MAX; wAreaIndex++)
        {
            int64_t delta_coin = player->m_lBetArray[wAreaIndex];
            if (delta_coin == 0)
                continue;

            delta_coin *= Multiple[wAreaIndex];

            if (cbWinArea[wAreaIndex] == TRUE)
            {
                player->m_lTempScore += delta_coin;
                banker->m_lTempScore -= delta_coin;

                if (banker->source == E_MSG_SOURCE_ROBOT && player->source != E_MSG_SOURCE_ROBOT) //机器人坐庄，真人玩家下注
                {
                    systemWinCount -= delta_coin;
                }

                if (banker->source != E_MSG_SOURCE_ROBOT && player->source == E_MSG_SOURCE_ROBOT) //真人坐庄，机器人下注
                {
                    systemWinCount += delta_coin;
                }
            }
            else
            {
                player->m_lTempScore -= delta_coin;
                banker->m_lTempScore += delta_coin;

                if (banker->source == E_MSG_SOURCE_ROBOT && player->source != E_MSG_SOURCE_ROBOT) //机器人坐庄，真人玩家下注
                {
                    systemWinCount += delta_coin;
                }

                if (banker->source != E_MSG_SOURCE_ROBOT && player->source == E_MSG_SOURCE_ROBOT) //真人坐庄，机器人下注
                {
                    systemWinCount -= delta_coin;
                }
            }
        }
    }

	int64_t lUserWin = 0;
    for (int i = 0; i < GAME_PLAYER; i++)
    {
        Player* player = this->player_array[i];
		if (!IProcess::CanPlay(player))
		{
			continue;
		}

        if (player->source == E_MSG_SOURCE_ROBOT)
        {
            lSystemScoreCount += player->m_lTempScore;
        }
		if (player->source != E_MSG_SOURCE_ROBOT)
		{
			lUserWin += player->m_lTempScore;
		}
    }

    {
        int64_t tempScore = lSystemScoreCount + bankerwincount;
        _LOG_ERROR_( "systemWinCount is :%ld, tempScore:%ld, lSystemScoreCount:%ld, lowerlimit:%ld, upperlimit:%ld " ,
                systemWinCount , 
                lSystemScoreCount , 
                tempScore, 
                this->coincfg.lowerlimit , 
                this->coincfg.upperlimit );

        if (tempScore > this->coincfg.lowerlimit && tempScore <= this->coincfg.upperlimit)
        {
            // 机器人当庄时必赢的情况
            if (m_NeedMakeRobotBankerWin && lUserWin > banker->m_lTempScore)
            {
                return false;
            }

            // 系统输钱，库存在安全范围，但库存单次降低超过 70%， 不予开奖
            if (lSystemScoreCount<0 && tempScore > 0 && bankerwincount > 0 && (tempScore * 100) / bankerwincount <= 40)
            {
                return false;
            }
			if (bankerwincount - lUserWin<=0)
			{
				return false;
			}
// 			if (lUserWin >= (bankerwincount*0.6))
// 			{
// 				return false;
// 			}

            return true;
        }

        if ( tempScore <= this->coincfg.lowerlimit )
        {
			if (bankerwincount - lUserWin <= 0)
			{
				return false;
			}
            //系统赢
            if (lSystemScoreCount > 0)
            {
                return true;
            }
            else
            {
                return false;
            }
        }

        if ( tempScore > this->coincfg.upperlimit )
        {
			if (bankerwincount - lUserWin <= 0)
			{
				return false;
			}
            //系统输
            if (lSystemScoreCount <= 0)
            {
                return true;
            }
            else
            {
                return false;
            }
        }

        return true;
    }
}

void Table::DisplayCards()
{
    BYTE cbPlayerCount = m_GameLogic.GetCardListPip(m_cbTableCardArray[INDEX_PLAYER] , m_cbCardCount[INDEX_PLAYER]);
    BYTE cbBankerCount = m_GameLogic.GetCardListPip(m_cbTableCardArray[INDEX_BANKER] , m_cbCardCount[INDEX_BANKER]);
    std::string card_more;
    std::string cardInfo1;
    std::string cardInfo2;

    char msg[32] = { 0 };
    if (m_cbCardCount[INDEX_PLAYER] == 3)
    {
        card_more = m_GameLogic.getCardInfo(m_cbTableCardArray[INDEX_PLAYER][2]);
        sprintf(msg , "补牌: %s, 总点数:%d" ,  card_more.c_str(), cbPlayerCount);
    }
    else
    {
        sprintf(msg , "总点数:%d" , cbPlayerCount);
    }
    cardInfo1 = m_GameLogic.getCardInfo(m_cbTableCardArray[INDEX_PLAYER][0]);
    cardInfo2 = m_GameLogic.getCardInfo(m_cbTableCardArray[INDEX_PLAYER][1]);
    _LOG_DEBUG_("闲[%s, %s], %s", cardInfo1.c_str(), cardInfo2.c_str(), msg);


    char msg2[32] = { 0 };
    if (m_cbCardCount[INDEX_BANKER] == 3)
    {
        card_more = m_GameLogic.getCardInfo(m_cbTableCardArray[INDEX_BANKER][2]);
        sprintf(msg2 , "补牌: %s, 总点数:%d" , card_more.c_str() , cbBankerCount);
    }
    else
    {
        sprintf(msg2 , "总点数:%d" , cbBankerCount);
    }
    cardInfo1 = m_GameLogic.getCardInfo(m_cbTableCardArray[INDEX_BANKER][0]);
    cardInfo2 = m_GameLogic.getCardInfo(m_cbTableCardArray[INDEX_BANKER][1]);

    _LOG_DEBUG_("庄[%s, %s], %s" , cardInfo1.c_str() , cardInfo2.c_str() , msg2);
}

// 选择庄家
void Table::ConfirmBanker()
{
	Player *curBanker = getBanker();
	if (curBanker == NULL)
	{
		return;
	}

	ULOGGER(E_LOG_INFO, curBanker->id) << "I'm banker, banked_cnt = " << bankernum << ", max_cnt = " << m_nMaxBankerNum;
	if (bankernum < m_nMaxBankerNum)
	{
		return;
	}

	// 清理掉已经不符合上庄条件的玩家
	std::list<Player*>::iterator iter = BankerList.begin();
	while (iter != BankerList.end())
	{
		Player *pItem = (*iter);
		if (pItem == NULL || pItem->m_lMoney < m_nBankerlimit)
		{
			BankerList.erase(iter++);
		}
		else
		{
			++iter;
		}
	}

	//上庄列表排序 (按资产降序排列)
	BankerList.sort(compare_nocase);

	size_t callnum = BankerList.size();
	Player* Banker = NULL;

	if (callnum > 0)
	{
		if (rand() % 100 < 60)
		{
			Banker = BankerList.front();
		}
		else
		{
			int banker_idx = rand() % callnum;
			std::list<Player*>::const_iterator citer = BankerList.begin();
			for (size_t idx = 0; idx < callnum; ++idx)
			{
				if (banker_idx == 0)
				{
					Banker = (*citer);
					break;
				}

				--banker_idx;
			}
		}
	}
	else
	{
		Banker = &m_robotbanker;
	}

	if (Banker->id == bankeruid)
	{
		LOGGER(E_LOG_INFO) << "banker not change!";
		//need init counter.
		//return;
	}

	this->rotateBanker(Banker);
}

void Table::GameStart()
{
	ConfirmBanker();

	for (int i = 0; i < GAME_PLAYER; i++)
	{
		Player * pCurrent = player_array[i];
		if (!IProcess::CanPlay(pCurrent))
		{
			continue;
		}

		pCurrent->reset();
		pCurrent->m_nStatus = STATUS_PLAYER_RSTART;
	}

	time_t t;
	time(&t);
	char time_str[32] = { 0 };
	struct tm* tp = localtime(&t);
	strftime(time_str, 32, "%Y%m%d%H%M%S", tp);
	char gameId[80] = { 0 };
	sprintf(gameId, "%s|%02d|%02d|%d|%d|%d|%d|%d", time_str, Configure::getInstance()->m_nLevel, getPlayerCount(false, true),
		player_array[1] ? player_array[1]->id : 0,
		player_array[2] ? player_array[2]->id : 0,
		player_array[3] ? player_array[3]->id : 0,
		player_array[4] ? player_array[4]->id : 0,
		player_array[5] ? player_array[5]->id : 0);
	this->setGameID(gameId);
	//LOGGER(E_LOG_INFO) << "[gameStart] tid=" << this->id << " ante=" << this-> << " ntax=" << this->m_nTax << " gameid=" << this->getGameID();

	bool canStart = isAllReady() && getPlayerCount(true, true) > 0;
	if (!canStart)
	{
		LOGGER(E_LOG_INFO) << "Can't start game right now!";
        if (!this->isActive())
        {
            this->m_nStatus = STATUS_TABLE_READY;
        }

		return;
	}

	this->m_nStatus = STATUS_TABLE_IDLE;
	this->reloadCfg();
	this->setStartTime(time(NULL));

	IProcess::GameStart(this);
	this->startIdleTimer(Configure::getInstance()->idletime);
	this->setStatusTime(time(NULL));
}

void Table::GameOver()
{
	m_nDespatchNum = 0;

	int64_t bankerwincount = 0;
	int64_t playerwincount = 0;
	int64_t userbankerwincount = 0;
	CoinConf::getInstance()->getWinCount(bankerwincount, playerwincount, userbankerwincount);

    m_NeedMakeRobotBankerWin = isMakeRobotBankerWin(this);

    SortedScordIndex.clear();

    DisplayPlayerBetInfo();

    // 根据下注情况，确定所有开奖情况的赔付，选择一个较优的开奖区域
    CalcualAllResult();

    srand(time(NULL));

	//_LOG_INFO_("========= bankerwincount == %ld\n", bankerwincount);
	//short resultret = CoinConf::getInstance()->getSwtichTypeResult();
	//int printresult = 0;
	while(true)
	{
		/*
		if(resultret > 0 && resultret < 4)
		{
			if(SysSetResult(resultret))
			{
				printresult = 1;
				break;
			}
		}
		else if (Loser && Configure::getInstance().randlose > 0 &&
			((Loser->id != bankeruid && Loser->m_lBetArray[0] >= Configure::getInstance().switchbetmoney) || Loser->id == bankeruid))
		{
			if(SetLoserResult(Loser))
			{
				printresult = 2;
				break;
			}
		}
		else
		*/
		//{
			//if(SetResult(bankerwincount, userbankerwincount))
			//	break;
		//}
        DispatchTableCard(rand());
        //if (SetResult(bankerwincount, userbankerwincount))
        if (SetResult(bankerwincount , userbankerwincount))
        {
            DisplayCards();
            break;
        }
            
		m_nDespatchNum++;
        if (m_nDespatchNum > 100)
        {
            BYTE winType = 0;
            BYTE zhuang_dui = 0;
            BYTE xian_dui = 0;

            OpenType2WinAreasInfo(GetSafeOpenType(), winType, zhuang_dui, xian_dui);

            DispatchTableCard(rand(), winType, zhuang_dui, xian_dui);

            DisplayCards();
            break;
        }
			
	}

	//_LOG_INFO_("GameOver: GameID[%s] dispatch num[%d] bet1:%ld bet2:%ld bet3:%ld Loser:%d printresult:%d resultret:%d bankeruid:%d\n", 
	//	this->getGameID(), m_nDespatchNum, this->m_lTabBetArray[1], this->m_lTabBetArray[2], this->m_lTabBetArray[3], Loser ? Loser->id : 0, printresult, resultret, bankeruid);
	
	//_LOG_INFO_("========= WinType == %d\n", m_bWinType);

	//_LOG_INFO_("GameOver: GameID[%s] dispatch num[%d] bet1:%ld bet2:%ld bet3:%ld bankeruid:%d\n",
	//	this->getGameID(), m_nDespatchNum, this->m_lTabBetArray[1], this->m_lTabBetArray[2], this->m_lTabBetArray[3], bankeruid);
    CalculateScore(0);
	IProcess::GameOver(this);
	ClearBetInfo();
}

bool Table::isAllReady()
{
	int readyCount = 0;
	int totalCount = getPlayerCount(true, true);

	for (int i = 0; i < GAME_PLAYER; ++i)
	{
		if (player_array[i] && player_array[i]->isReady())
			readyCount++;
	}

	LOGGER(E_LOG_INFO) << "readyCount = " << readyCount << " totalCount = " << totalCount;
	return (readyCount >= totalCount) ? true : false;
}

int64_t Table::RobotBankerGetSystemWin(BYTE openType)
{
    int64_t system_win = 0;

    for (int j = 0; j < GAME_PLAYER; ++j)
    {
        Player *player = this->player_array[j];
        if (player == NULL)
            continue;

        if (player->id == this->bankeruid)
            continue;

        if (player->source == E_MSG_SOURCE_ROBOT)    // 只考虑真人闲家跟系统之间的输赢
            continue;

        for (BYTE wAreaIndex = 1; wAreaIndex < AREA_MAX; wAreaIndex++)
        {
            if (AllWinArea[openType][wAreaIndex] == TRUE)
            {
                system_win -= player->m_lBetArray[wAreaIndex] * Multiple[wAreaIndex];   // 玩家压中， 按倍率和玩家下注 进行赔付
            }
            else
            {
                system_win += player->m_lBetArray[wAreaIndex];                          // 玩家未压中，下注被系统吃掉
            }
        }
    }

    return system_win;
}


int64_t Table::PlayerBankerGetSystemWin(BYTE openType)
{
    int64_t system_win = 0;

    for (int j = 0; j < GAME_PLAYER; ++j)
    {
        Player *player = this->player_array[j];
        if (player == NULL)
            continue;

        if (player->id == this->bankeruid)
            continue;

        if (player->source != E_MSG_SOURCE_ROBOT)    // 只考虑机器人闲家跟真人庄家之间的输赢
            continue;

        for (BYTE wAreaIndex = 1; wAreaIndex < AREA_MAX; wAreaIndex++)
        {
            if (AllWinArea[openType][wAreaIndex] == TRUE)
            {
                system_win += player->m_lBetArray[wAreaIndex] * Multiple[wAreaIndex];   // 机器人压中， 按倍率和下注 交给系统
            }
            else
            {
                system_win -= player->m_lBetArray[wAreaIndex];                          // 机器人未压中，下注被玩家吃掉
            }
        }
    }

    return system_win;
}

void Table::SortOpenTypes()
{
    
    for (BYTE i = 0; i < 12; ++i)
    {
        ScoreIndexRecord item;
        item.index = i;
        item.score = AllOpenResult[i];
        SortedScordIndex.push_back(item);
    }

    sort(SortedScordIndex.begin() , SortedScordIndex.end());

    _LOG_ERROR_("-------------------- round all open result ---------------------------------------");
    std::vector<ScoreIndexRecord>::iterator it = SortedScordIndex.begin();
    for (; it != SortedScordIndex.end(); ++ it)
    {
        _LOG_ERROR_("index: %d, system win :%d", it->index, it->score);
    }
    _LOG_ERROR_("-------------------- ---------------------  ---------------------------------------");
}

BYTE Table::GetSafeOpenType()
{
    BYTE openTypeIndex[12] = {0};
    BYTE count = 0;
    std::vector<ScoreIndexRecord>::iterator it = SortedScordIndex.begin();
    for (; it != SortedScordIndex.end(); ++it)
    {
        if (it->score >= 0)
        {
            openTypeIndex[count++] = (it->index);
        }
    }

    if (count == 0)
    {
        return 8;
    }

    return openTypeIndex[rand() % count];
}

void Table::OpenType2WinAreasInfo(BYTE openType , BYTE &winType ,  BYTE &zhuang_dui ,  BYTE &xian_dui)
{
    zhuang_dui = 0;
    xian_dui = 0;
    switch (openType)
    {
    case 0:
        winType = WIN_TYPE_ZHUANG;
        break;
    case 1:
        winType = WIN_TYPE_ZHUANG;
        zhuang_dui = 1;
    case 2:
        winType = WIN_TYPE_ZHUANG;
        xian_dui = 1;
        break;
    case 3:
        winType = WIN_TYPE_ZHUANG;
        zhuang_dui = 1;
        xian_dui = 1;
    case 4:
        winType = WIN_TYPE_XIAN;
        break;
    case 5:
        winType = WIN_TYPE_XIAN;
        xian_dui = 1;
    case 6:
        winType = WIN_TYPE_XIAN;
        zhuang_dui = 1;
        break;
    case 7:
        winType = WIN_TYPE_XIAN;
        zhuang_dui = 1;
        xian_dui = 1;
    case 8:
        winType = WIN_TYPE_HE;
        break;
    case 9:
        winType = WIN_TYPE_HE;
        zhuang_dui = 1;
    case 10:
        winType = WIN_TYPE_HE;
         xian_dui = 1;
        break;
    case 11:
        winType = WIN_TYPE_HE;
        zhuang_dui = 1;
        xian_dui = 1;
        break;
    default:
        winType = WIN_TYPE_ZHUANG;
    }
}

void Table::DisplayPlayerBetInfo()
{
    _LOG_ERROR_("庄家: %d", bankeruid);
    for ( int32_t i = 0; i < GAME_PLAYER; ++i )
    {
        Player* player = player_array[i];
        if (player)
        {
            _LOG_ERROR_("%s, %s, %s, %d: 压庄[%d], 压闲[%d], 压和[%d], 压庄对[%d], 压闲对[%d]" ,
                bankeruid == player->id ? "庄家" : "闲家" ,
                player->source == E_MSG_SOURCE_ROBOT ? "机器人" : "真人" ,
                player->name, player->id,
                player->m_lBetArray[AREA_ZHUANG] ,
                player->m_lBetArray[AREA_XIAN] ,
                player->m_lBetArray[AREA_PING] ,
                player->m_lBetArray[AREA_ZHUANG_DUI] ,
                player->m_lBetArray[AREA_XIAN_DUI]);
        }
    }
}


void Table::CalcualAllResult()
{
    Player* banker = NULL;
    {
        banker = this->getBanker();
    }

    if (banker == NULL)
    {
        return;
    }

    bool isBankerRobot = (banker->source == E_MSG_SOURCE_ROBOT? true:false);

    for (BYTE i = 0; i < 12; ++i) //统计每种开奖的
    {
        if (isBankerRobot) //系统坐庄
        {
            AllOpenResult[i] = RobotBankerGetSystemWin(i);
        }
        else
        {
            AllOpenResult[i] = PlayerBankerGetSystemWin(i);
        }
    }

    SortOpenTypes();
}

void Table::CalculateScore(short resultret)
{
    //计算牌点
    BYTE cbPlayerCount = m_GameLogic.GetCardListPip( m_cbTableCardArray[INDEX_PLAYER],m_cbCardCount[INDEX_PLAYER] );
    BYTE cbBankerCount = m_GameLogic.GetCardListPip( m_cbTableCardArray[INDEX_BANKER],m_cbCardCount[INDEX_BANKER] );
    //系统输赢
    int64_t lSystemScoreCount = 0;
    //推断玩家
    BYTE cbWinArea[AREA_MAX] = {FALSE};
    DeduceWinner(cbWinArea);

	Player* banker = NULL;
	banker = this->getBanker();

	if(banker == NULL)
	{
		//_LOG_ERROR_("CalculateScore: banker is NULL bankersid:%d bankeruid:%d\n", bankersid, bankeruid);
		return;
	}
    
    m_bWinType = getWinType(cbWinArea);
    m_cbBankerPair = cbWinArea[AREA_ZHUANG_DUI];
    m_cbPlayerPair = cbWinArea[AREA_XIAN_DUI];
    m_cbBankerPoint = cbBankerCount;
    m_cbPlayerPoint = cbPlayerCount;

	//游戏记录
	ServerGameRecord &GameRecord = m_GameRecordArrary[m_nRecordLast];
    GameRecord.cbBankerCount = m_cbBankerPoint;
    GameRecord.cbPlayerCount = m_cbPlayerPoint;
    GameRecord.bPlayerTwoPair = m_cbPlayerPair;
    GameRecord.bBankerTwoPair = m_cbBankerPair;
    GameRecord.cbWinType = m_bWinType;
    //if ( cbWinArea[AREA_TONG_DUI] == TRUE )
    //    GameRecord.cbKingWinner = AREA_TONG_DUI;
    LOGGER(E_LOG_INFO) << "wintype = " << m_bWinType
                       << "banker point = " << m_cbBankerPoint
                       << "player point = " << m_cbPlayerPoint
                       << "banker pair = " << m_cbBankerPair
                       << "player pair = " << m_cbPlayerPair;
    
    for(int j = 0; j < GAME_PLAYER; ++j)
	{
		Player *player = this->player_array[j];
		if (!IProcess::CanPlay(player)) continue;

		if (player->id == this->bankeruid) continue;

        for (BYTE wAreaIndex = 1; wAreaIndex < AREA_MAX; wAreaIndex++)
        {
            if (cbWinArea[wAreaIndex] == TRUE)
            {
				if (wAreaIndex == AREA_PING|| wAreaIndex == AREA_XIAN_DUI || wAreaIndex == AREA_ZHUANG_DUI)
				{
					player->m_lWinScore += player->m_lBetArray[wAreaIndex] * (Multiple[wAreaIndex] - 1);
					banker->m_lWinScore -= player->m_lBetArray[wAreaIndex] * (Multiple[wAreaIndex] - 1);
					this->m_lBankerWinScoreCount -= player->m_lBetArray[wAreaIndex] * (Multiple[wAreaIndex] - 1);
				}
				else
				{
					player->m_lWinScore += player->m_lBetArray[wAreaIndex] * Multiple[wAreaIndex];
					banker->m_lWinScore -= player->m_lBetArray[wAreaIndex] * Multiple[wAreaIndex];
					this->m_lBankerWinScoreCount -= player->m_lBetArray[wAreaIndex] * Multiple[wAreaIndex];
				}
            }
			else
			{
				if (cbWinArea[AREA_PING] != TRUE)
				{
					player->m_lLostScore -= player->m_lBetArray[wAreaIndex];
					banker->m_lWinScore += player->m_lBetArray[wAreaIndex];
					this->m_lBankerWinScoreCount += player->m_lBetArray[wAreaIndex];
				}
				else
				{
					if (wAreaIndex == AREA_XIAN_DUI || wAreaIndex == AREA_ZHUANG_DUI)
					{
						player->m_lLostScore -= player->m_lBetArray[wAreaIndex];
						banker->m_lWinScore += player->m_lBetArray[wAreaIndex];
						this->m_lBankerWinScoreCount += player->m_lBetArray[wAreaIndex];
					}
				}
			}
            
//             if (cbWinArea[AREA_PING] != TRUE) //如果和赢，则其他位置的下注都无需扣除
//             {
//                player->m_lLostScore -= player->m_lBetArray[wAreaIndex];
//                 banker->m_lWinScore += player->m_lBetArray[wAreaIndex];
//                 this->m_lBankerWinScoreCount += player->m_lBetArray[wAreaIndex];
//             }
        }
	}
    
    //移动下标
    m_nRecordLast = (m_nRecordLast+1) % MAX_SCORE_HISTORY;
	
    
    if (m_historyCount < MAX_SCORE_HISTORY)
    {
		m_historyCount++;
        
    }
	else
	{
		m_historyCount = MAX_SCORE_HISTORY;
	}
    
    int64_t lsysrobotcount = 0;
    int64_t bankerwincount = 0;
    for(int j = 0; j < GAME_PLAYER; ++j)
    {
        Player *player = this->player_array[j];
        if (player==NULL) continue;
        
        //player->m_lWinScore += player->m_lLostScore;
        
        if (player->m_lWinScore > 0)
        {
            //Util::taxDeduction(player->m_lWinScore, this->m_nTax, player->tax);

            int64_t needTaxMoney = player->m_lWinScore + player->m_lLostScore;
            if (needTaxMoney > 0)
            {
				GameUtil::CalcSysWinMoney(needTaxMoney, player->tax, this->m_nTax);
                player->m_lWinScore -= player->tax;
            }
            else
            {
                player->tax = 0;
            }
            
            if (E_MSG_SOURCE_ROBOT == player->source)
            {
                int64_t lRobotWin = player->m_lWinScore + player->tax + player->m_lLostScore;
                lsysrobotcount += lRobotWin;
				if (!RedisLogic::AddRobotWin(Tax(), player->pid, Configure::getInstance()->m_nServerId, (int)lRobotWin))
				{
					LOGGER(E_LOG_WARNING) << "OperationRedis::AddRobotWin Error, pid=" << player->pid
						<< ", m_nServerId=" << Configure::getInstance()->m_nServerId << ", win="
						<< lRobotWin;
				}
            }
            else
            {
                if (player->id == bankeruid)
                    this->m_lBankerWinScoreCount -= player->tax;
                
				if (!RedisLogic::UpdateTaxRank(Tax(), player->pid, Configure::getInstance()->m_nServerId,
							GAME_ID, Configure::getInstance()->m_nLevel,
							player->tid, player->id, player->tax)) {
					LOGGER(E_LOG_WARNING) << "OperationRedis::GenerateTip Error, pid=" << player->pid
						<< ", m_nServerId=" << Configure::getInstance()->m_nServerId << ", gameid="
						<< GAME_ID << ", level=" << Configure::getInstance()->m_nLevel << ", id="
						<< player->id << ", Tax=" << player->tax;
				}
            }
        }
        else //机器人输钱也需要算
        {
            if (player->source == E_MSG_SOURCE_ROBOT)
            {
                lsysrobotcount += player->m_lWinScore + player->m_lLostScore;
            }
        }
        
        player->m_lMoney += player->m_lWinScore + player->m_lReturnScore + player->m_lLostScore;
    }
    
    if(banker->source != E_MSG_SOURCE_ROBOT)
    {
        bankerwincount = banker->m_lWinScore + banker->tax;
    }
    
    if(resultret > 0)
    {
        int notInsertRedis = 0;
    }
    else
    {
        if(lsysrobotcount != 0)
        {
            CoinConf::getInstance()->setWinCount(lsysrobotcount, 0, bankerwincount);
        }
    }
    
    ++bankernum;
}

void Table::reloadCfg()
{
	CoinConf* coinCalc = CoinConf::getInstance();
	Cfg* coincfg = coinCalc->getCoinCfg();
	m_nTax = coincfg->tax;
	m_nLimitCoin = coincfg->limitcoin;
	m_nRetainCoin = coincfg->retaincoin;
	m_nBankerlimit = coincfg->bankerlimit;
	m_nMaxBankerNum = coincfg->bankernum;
	m_nMaxBetNum = coincfg->betnum;
    m_nMinBetNum = coincfg->minbetnum;
	this->m_nPersonLimitCoin = coincfg->PersonLimitCoin;
    for (int i = 0; i < CHIP_COUNT; i++)
    {
        m_nChipArray[i] = coincfg->chiparray[i];
    }

    coinCalc->getRewardRates(m_nZhuangWinRate , m_nXianWinRate , m_nPingRate , m_nZhuangDuiRate , m_nXianDuiRate);
}

//==================================TableTimer=================================================
void Table::startIdleTimer(int timeout)
{
	timer.startIdleTimer(timeout);
}

void Table::stopIdleTimer()
{
	timer.stopIdleTimer();
}

void Table::startBetTimer(int timeout)
{
	timer.startBetTimer(timeout);
}

void Table::stopBetTimer()
{
	timer.stopBetTimer();
}

void Table::startOpenTimer(int timeout)
{
	timer.startOpenTimer(timeout);
}

void Table::stopOpenTimer()
{
	timer.stopOpenTimer();
}

bool Table::EnterSeat(int seatid, Player *player)
{
    if (seatid > MAX_SEAT_NUM || seatid == 0)
    {
        return false;
    }

    if (m_SeatidArray[seatid-1] != NULL) 
    {
        return false;
    }

    if (player->m_nSeatID != 0)
    {
        LeaveSeat(player->m_nSeatID);
    }

    player->m_nSeatID = seatid;
    m_SeatidArray[seatid - 1] = player;

	return true;
}

bool Table::LeaveSeat(int seatid)
{
    if (seatid > MAX_SEAT_NUM || seatid == 0)
    {
        return false;
    }
    
    if (m_SeatidArray[seatid-1] != NULL)
    {
        m_SeatidArray[seatid-1]->m_nSeatID = 0;
        m_SeatidArray[seatid-1] = NULL;
        return true;
    }
    return false;
}

void Table::SendSeatInfo(Player *player)
{
    _LOG_INFO_("Table::SendSeatInfo : player->uid=[%d]\n", player->id);
    if (player == NULL)
    {
        _LOG_ERROR_("Table::SendSeatInfo : why player is null.\n");
        return;
    }

	if (player->source == E_MSG_SOURCE_ROBOT)
	{
		return;
	}
    
    OutputPacket response;
    response.Begin(CLIENT_MSG_REFRESH_SEATLIST, player->id);
    response.WriteShort(0);
    response.WriteString("");
    response.WriteInt(player->id);
    response.WriteInt((Configure::getInstance()->m_nServerId << 16) | this->id);
    
    response.WriteInt(MAX_SEAT_NUM); //一共多少桌位号
    
    for (int i = 0; i < MAX_SEAT_NUM; ++i)
    {    
        if (m_SeatidArray[i] != NULL)
        {
            response.WriteInt(i+1);                            //座位ID
            response.WriteInt(m_SeatidArray[i]->id);        //玩家ID
            response.WriteString(m_SeatidArray[i]->name);    //玩家名称
            response.WriteString(m_SeatidArray[i]->headlink);//玩家头像url
            response.WriteInt64(m_SeatidArray[i]->m_lMoney);                    //玩家金币
        } else {
            response.WriteInt(i+1);      //座位ID
            response.WriteInt(0);         //玩家ID
            response.WriteString("");     //玩家名称
            response.WriteString("");     //玩家头像url
            response.WriteInt64(0);         //玩家金币
        }    
    }

    for (int i = 0; i < MAX_SEAT_NUM; ++i)
    {
        if (m_SeatidArray[i] != NULL)
        {
            response.WriteString(m_SeatidArray[i]->json);      //player json
        }
        else
        {
            response.WriteString("");      //player json
        }
    }

    response.End();
    HallManager::getInstance()->sendToHall(player->m_nHallid, &response, false);
}

void Table::NotifyPlayerSeatInfo()
{
    _LOG_INFO_("Table::NotifyPlayerSeatInfo");
    for(int i = 0; i < GAME_PLAYER; ++i)
    {
        if (this->player_array[i])
        {
            SendSeatInfo(this->player_array[i]);
        }
    }
}

void Table::DispatchTableCard(int randSeed, BYTE type, BYTE zhuang_dui, BYTE xian_dui)
{
	if (!receivePhpPush)
	{
        if (type > 0)
        {
            RandCardByAssignedType(randSeed , type);
        }
        else  //随机扑克
        {
            RandCardByWinType(randSeed);
        }
	}
	else
	{
		phpRandCard(randSeed);
	}
    //首次发牌
    m_cbCardCount[INDEX_PLAYER] = 2;
    m_cbCardCount[INDEX_BANKER] = 2;
    //计算点数
    BYTE cbBankerCount = m_GameLogic.GetCardListPip(m_cbTableCardArray[INDEX_BANKER], m_cbCardCount[INDEX_BANKER]);
    BYTE cbPlayerTwoCardCount = m_GameLogic.GetCardListPip(m_cbTableCardArray[INDEX_PLAYER], m_cbCardCount[INDEX_PLAYER]);
    //闲家补牌
    BYTE cbPlayerThirdCardValue = 0; //第三张牌点数
    if (cbPlayerTwoCardCount <= 5 && cbBankerCount < 8)
    {
        //计算点数
        m_cbCardCount[INDEX_PLAYER]++;
        cbPlayerThirdCardValue = m_GameLogic.GetCardPip(m_cbTableCardArray[INDEX_PLAYER][2]);
    }
    //庄家补牌
    if (cbPlayerTwoCardCount < 8 && cbBankerCount < 8)
    {
        switch(cbBankerCount)
        {
            case 0:
            case 1:
            case 2:
                m_cbCardCount[INDEX_BANKER]++;
                break;
                
            case 3:
                if((m_cbCardCount[INDEX_PLAYER] == 3 && cbPlayerThirdCardValue!=8) || m_cbCardCount[INDEX_PLAYER] == 2) m_cbCardCount[INDEX_BANKER]++;
                break;
                
            case 4:
                if((m_cbCardCount[INDEX_PLAYER] == 3 && cbPlayerThirdCardValue!=1 && cbPlayerThirdCardValue!=8 && cbPlayerThirdCardValue!=9 && cbPlayerThirdCardValue!=0) || m_cbCardCount[INDEX_PLAYER] == 2) m_cbCardCount[INDEX_BANKER]++;
                break;
                
            case 5:
                if((m_cbCardCount[INDEX_PLAYER] == 3 && cbPlayerThirdCardValue!=1 && cbPlayerThirdCardValue!=2 && cbPlayerThirdCardValue!=3  && cbPlayerThirdCardValue!=8 && cbPlayerThirdCardValue!=9 &&  cbPlayerThirdCardValue!=0) || m_cbCardCount[INDEX_PLAYER] == 2) m_cbCardCount[INDEX_BANKER]++;
                break;
                
            case 6:
                if(m_cbCardCount[INDEX_PLAYER] == 3 && (cbPlayerThirdCardValue == 6 || cbPlayerThirdCardValue == 7)) m_cbCardCount[INDEX_BANKER]++;
                break;
                
                //不须补牌
            case 7:
            case 8:
            case 9:
                break;
            default:
                break;
        }
    }

    m_RemainCardCount -= m_cbCardCount[INDEX_PLAYER];
    m_RemainCardCount -= m_cbCardCount[INDEX_BANKER];
}

void Table::DeduceWinner(BYTE *pWinArea)
{
    //计算牌点
    BYTE cbPlayerCount = m_GameLogic.GetCardListPip(m_cbTableCardArray[INDEX_PLAYER],m_cbCardCount[INDEX_PLAYER]);
    BYTE cbBankerCount = m_GameLogic.GetCardListPip(m_cbTableCardArray[INDEX_BANKER],m_cbCardCount[INDEX_BANKER]);
    //胜利区域--------------------------
    //平
    if( cbPlayerCount == cbBankerCount )
    {
        pWinArea[AREA_PING] = TRUE;
        //同点平
        if (m_cbCardCount[INDEX_PLAYER] == m_cbCardCount[INDEX_BANKER])
        {
            WORD wCardIndex = 0;
            for (; wCardIndex < m_cbCardCount[INDEX_PLAYER]; ++wCardIndex )
            {
                BYTE cbBankerValue = m_GameLogic.GetCardValue(m_cbTableCardArray[INDEX_BANKER][wCardIndex]);
                BYTE cbPlayerValue = m_GameLogic.GetCardValue(m_cbTableCardArray[INDEX_PLAYER][wCardIndex]);
                if ( cbBankerValue != cbPlayerValue ) break;
            }
            
            if ( wCardIndex == m_cbCardCount[INDEX_PLAYER] )
            {
                pWinArea[AREA_PING] = TRUE;
            }
        }
    }
    // 庄
    else if ( cbPlayerCount < cbBankerCount)
    {
        pWinArea[AREA_ZHUANG] = TRUE;
        
        //天王判断
        if ( cbBankerCount == 8 || cbBankerCount == 9 )
        {
            //pWinArea[AREA_ZHUANG_TIAN] = TRUE;
        }
    }
    // 闲
    else
    {
        pWinArea[AREA_XIAN] = TRUE;
        
        //天王判断
        if ( cbPlayerCount == 8 || cbPlayerCount == 9 )
        {
            //pWinArea[AREA_XIAN_TIAN] = TRUE;
        }
    }
    //对子判断
    if (m_GameLogic.GetCardValue(m_cbTableCardArray[INDEX_PLAYER][0]) == m_GameLogic.GetCardValue(m_cbTableCardArray[INDEX_PLAYER][1]))
    {
        pWinArea[AREA_XIAN_DUI] = TRUE;
    }
    if (m_GameLogic.GetCardValue(m_cbTableCardArray[INDEX_BANKER][0]) == m_GameLogic.GetCardValue(m_cbTableCardArray[INDEX_BANKER][1]))
    {
        pWinArea[AREA_ZHUANG_DUI] = TRUE;
    }
}
    
BetArea::BetArea(BET_TYPE bt, Table* tab)
{
    this->betType = bt;
    this->table = tab;
    this->gameRecord = 0;
    this->betLimit = 0;
    this->betMoney = 0;
    this->betRealMoney = 0;
}

void BetArea::updateBetLimit()
{
    
}

void BetArea::updateBetMoney(Player *p)
{
    if (p == NULL)
    {
        return;
    }
    //this->betMoney += p->be
}

void BetArea::updateRealMoney(Player *p)
{
    
}
