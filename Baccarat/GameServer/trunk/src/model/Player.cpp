#include <json/json.h>
#include "Player.h"
#include "Configure.h"
#include "AllocSvrConnect.h"
#include "Logger.h"
#include "GameCmd.h"
#include "GameApp.h"
#include "MoneyAgent.h"
#include "CoinConf.h"
#include "IProcess.h"
#include "Protocol.h"
#include "JsonWrapper.h"

void Player::init()
{
	id = 0;
	memset(name, 0, sizeof(name));
	memset(json, 0, sizeof(json));
	m_nStatus = 0;
	m_nHallid = 0;
	tid = -1;
	source = 0;
	m_bisCall = 0;
	isonline = true;
	m_bhasOpen = false;
	m_nWin = 0;
	m_nLose = 0;
	m_nRunAway = 0;
	m_nTie = 0;
	isCanlcebanker = false;
	m_lWinScore = 0;
	m_lReturnScore = 0;
	tax = 0;
	//memset(m_lResultArray, 0, sizeof(m_lResultArray));
	memset(m_lBetArray, 0, sizeof(m_lBetArray));
	m_lTempScore = 0;
	m_lLostScore = 0;
	m_nLastBetTime = 0;
}

void Player::reset()
{
	m_lWinScore = 0;
	m_lReturnScore = 0;
	m_bisCall = 0;
	tax = 0;
	m_lLostScore = 0;
	memset(m_lBetArray, 0, sizeof(m_lBetArray));
	//memset(m_lResultArray, 0, sizeof(m_lResultArray));
}

void Player::login()
{
	short clevel = Configure::getInstance()->m_nLevel;
	isonline = true;

	if(strlen(json)>=2 && json[0]=='{')
	{
		Json::Reader reader;
		Json::Value value;
		if (reader.parse(json, value))
		{   
			this->cid = JsonWrapper::parseJson(value,"cid",0);
			this->sid = JsonWrapper::parseJson(value,"sid",0); 
			this->pid = JsonWrapper::parseJson(value,"pid",0);
			this->gid = JsonWrapper::parseJson(value,"gameid",0);
			//是机器人则用传过来的战绩
			//if(source == 30)
			//{
				int totalNum = JsonWrapper::parseJson(value,"totalNum",20);
				int winNum = JsonWrapper::parseJson(value,"winNum",5);
				string link = JsonWrapper::parseJson(value,"picUrl","http://192.126.114.225/ucenter/data/icon/manphoto.png");
				strcpy(this->headlink, link.c_str());
				this->m_nWin = winNum;
				this->m_nLose = totalNum - winNum;
			//}
		} 
	}
	else
	{
		//_LOG_ERROR_("json parse error [%s]\n", json);
	}

	AllocSvrConnect::getInstance()->userEnterGame(this);
	this->setEnterTime(time(NULL));
	this->m_nLastBetTime = time(NULL);
}

void Player::standup()
{
	this->m_nStatus = STATUS_PLAYER_STANDUP;
}

void Player::sitdown()
{
	this->m_nStatus = STATUS_PLAYER_RSTART;
}

void Player::leave(bool isSendToUser)
{
	if(isSendToUser)
	{
		AllocSvrConnect::getInstance()->userLeaveGame(this);
	}
	IProcess::UpdateDBActiveTime(this);
	OutputPacket respone;
	respone.Begin(UPDATE_LEAVE_GAME_SERVER);
	respone.WriteInt(this->id);
	respone.End();
	if(this->source != 30)
	{
		if (MoneyServer()->Send(&respone) < 0)
		{
			ULOGGER(E_LOG_ERROR, this->id) << "Send request to MoneyServer Error";
		}
	}
	IProcess::UpdateDBActiveTime(this);
	this->m_nStatus = STATUS_PLAYER_LOGOUT;
	this->tid = -1;
	this->m_nSeatID = -1;

    // 退出的时候, 清空下注信息, 此时的下注信息不参与结算
    memset(m_lBetArray, 0, sizeof(m_lBetArray));
}

void Player::enter()
{
	AllocSvrConnect::getInstance()->userUpdateStatus(this, this->m_nStatus);
	this->setEnterTime(time(NULL));
}

bool Player::notBetCoin()
{
	ULOGGER(E_LOG_INFO, id) << "bet array[0] = " << m_lBetArray[0];
	if(m_lBetArray[0] != 0)
		return false;
	return true;
}

