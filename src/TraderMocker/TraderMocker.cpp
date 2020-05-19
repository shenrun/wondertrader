#include "TraderMocker.h"

#include "../Share/WTSParams.hpp"
#include "../Share/WTSDataDef.hpp"
#include "../Share/WTSTradeDef.hpp"
#include "../Share/WTSContractInfo.hpp"
#include "../Share/IBaseDataMgr.h"
#include "../Share/TimeUtils.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/decimal.h"
#include "../Share/WTSError.hpp"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

extern "C"
{
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderMocker *instance = new TraderMocker();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			delete trader;
			trader = NULL;
		}
	}
}

std::vector<uint32_t> splitVolumn(uint32_t vol, uint32_t minQty = 1, uint32_t maxQty = 100)
{
	uint32_t length = maxQty - minQty + 1;
	std::vector<uint32_t> ret;
	if (vol <= minQty)
	{
		ret.push_back(vol);
	}
	else
	{
		uint32_t left = vol;
		srand((uint32_t)time(NULL));
		while (left > 0)
		{
			uint32_t curVol = minQty + (uint32_t)rand() % length;

			if (curVol >= left)
				curVol = left;

			if (curVol == 0)
				continue;

			ret.push_back(curVol);
			left -= curVol;
		}
	}

	return ret;
}
TraderMocker::TraderMocker()
	: _terminated(false)
	, _listener(NULL)
	, _ticks(NULL)
	, _orders(NULL)
	, _trades(NULL)
	, _b_socket(NULL)
	, _max_tick_time(0)
	, _last_match_time(0)
{
	_auto_order_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 100);
	_auto_trade_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 300);
	_auto_entrust_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 100);
}


TraderMocker::~TraderMocker()
{
	if (_orders)
		_orders->release();

	if (_trades)
		_trades->release();

	if (_ticks)
		_ticks->release();
}

uint32_t TraderMocker::makeTradeID()
{
	return ++_auto_trade_id;
}

uint32_t TraderMocker::makeOrderID()
{
	return ++_auto_order_id;
}

bool TraderMocker::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	try
	{
		memset(buffer, 0, length);
		sprintf(buffer, "me.%u.%u.%u", TimeUtils::getCurDate(), _mocker_id, _auto_entrust_id++);
		return true;
	}
	catch (...)
	{

	}

	return false;
}

int TraderMocker::orderInsert(WTSEntrust* entrust)
{	
	if (entrust == NULL)
	{
		return 0;
	}

	entrust->retain();
	_io_service.post([this, entrust](){
		BoostUniqueLock lock(_mutex_api);

		WTSContractInfo* ct = _bd_mgr->getContract(entrust->getCode(), entrust->getExchg());

		/*
		 *	1������������
		 *	2��ƽ��Ҫ�ȼ���ƽ
		 *	3�����ͨ���ˣ�ƽ�ֻ�Ҫ����ֲ�
		 *	4����Ҫ���ǹ����ڻ�������
		 */

		bool bPass = false;
		std::string msg;
		do 
		{
			if (ct == NULL)
			{
				bPass = false;
				msg = "Ʒ�ֲ�����";
				break;
			}
			WTSCommodityInfo* commInfo = _bd_mgr->getCommodity(ct);

			//���۸����͵ĺϷ���
			if (entrust->getPriceType() == WPT_ANYPRICE && commInfo->getPriceMode() == PM_Limit)
			{
				bPass = false;
				msg = "�۸����Ͳ��Ϸ�";
				break;
			}

			//��������ĺϷ���
			if ((commInfo->getCategoty() == CC_Stock) && (entrust->getOffsetType() == WOT_OPEN) && !decimal::eq(decimal::mod(entrust->getVolumn(), 100), 0))
			{
				bPass = false;
				msg = "��Ʊ������������Ϊ100��������";
				break;
			}

			//��鷽��ĺϷ���
			if(commInfo->getCategoty() == CC_Stock && entrust->getDirection() == WDT_SHORT)
			{
				bPass = false;
				msg = "��Ʊ��������";
				break;
			}

			//���۸�ĺϷ���
			if(!decimal::eq(entrust->getPrice(), 0))
			{
				double pricetick = commInfo->getPriceTick();
				double v = entrust->getPrice() / pricetick;

				if (!decimal::eq(decimal::mod(entrust->getPrice(), pricetick), 0))	//�����ļ�鷽ʽ����С������õ��̣�Ȼ����ȡ���Ժ��ٸ�ԭ������������������0������������������
				{
					bPass = false;
					msg = "ί�м۸񲻺Ϸ�";
					break;
				}
			}


			//����ֱ��ͨ����������ʽ�
			if (entrust->getOffsetType() == WOT_OPEN)
			{
				bPass = true;
				break;
			}

			//�������Ҫ��ƽ����ֱ��ͨ������Ҫ��Թ����ڻ�
			if (commInfo->getCoverMode() == CM_None)
			{
				bPass = true;
				break;
			}
			
			//�������ƽ��ƽ�񣬶�ί�е���ƽ����ֱ�Ӿܾ�����ΪmockerΪ�˼򻯴��������������
			if (commInfo->getCoverMode() == CM_CoverToday && (entrust->getOffsetType() == WOT_CLOSE || entrust->getOffsetType() == WOT_CLOSEYESTERDAY))
			{
				bPass = false;
				msg = "û���㹻�Ŀ�ƽ��λ";
				break;
			}

			//���û�гֲֻ��ֲֲ߳�����ҲҪ
			auto it = _positions.find(ct->getFullCode());
			if(it == _positions.end())
			{
				bPass = false;
				msg = "û���㹻�Ŀ�ƽ��λ";
				break;
			}

			PosItem& pItem = it->second;
			bool isLong = entrust->getDirection() == WDT_LONG;

			double validQty = isLong ? (pItem._long._volumn - pItem._long._frozen) : (pItem._short._volumn - pItem._short._frozen);
			if(decimal::lt(validQty, entrust->getVolumn()))
			{
				bPass = false;
				msg = "û���㹻�Ŀ�ƽ��λ";
				break;
			}

			//����ֲ�
			if(isLong)
			{
				pItem._long._frozen += entrust->getVolumn();
			}
			else
			{
				pItem._short._frozen += entrust->getVolumn();
			}

			bPass = true;
			msg = "�µ��ɹ�";

		} while (false);
		
		if(bPass)
		{
			WTSOrderInfo* ordInfo = WTSOrderInfo::create();
			ordInfo->setCode(entrust->getCode());
			ordInfo->setExchange(entrust->getExchg());
			ordInfo->setDirection(entrust->getDirection());
			ordInfo->setOffsetType(entrust->getOffsetType());
			ordInfo->setUserTag(entrust->getEntrustID());
			ordInfo->setPrice(entrust->getPrice());
			char str[64];
			sprintf(str, "mo.%u.%u", _mocker_id, makeOrderID());
			ordInfo->setOrderID(str);
			ordInfo->setStateMsg(msg.c_str());
			ordInfo->setOrderState(WOS_NotTraded_Queuing);
			ordInfo->setOrderTime(TimeUtils::getLocalTimeNow());
			ordInfo->setVolumn(entrust->getVolumn());
			ordInfo->setVolLeft(entrust->getVolumn());
			ordInfo->setPriceType(entrust->getPriceType());
			ordInfo->setTimeCondition(entrust->getTimeCondition());

			if (_listener != NULL)
			{
				_listener->onRspEntrust(entrust, NULL);
				_listener->onPushOrder(ordInfo);
			}

			_codes.insert(ct->getFullCode());

			if (_orders == NULL)
				_orders = WTSArray::create();
			_orders->append(ordInfo, false);

			if (_awaits)
				_awaits = OrderCache::create();

			_awaits->add(ordInfo->getOrderID(), ordInfo, true);

			save_positions();
		}
		else
		{
			WTSError* err = WTSError::create(WEC_ORDERINSERT, msg.c_str());
			if (_listener != NULL)
			{
				_listener->onRspEntrust(entrust, err);
			}
			err->release();
		}
		entrust->release();
	});

	return 0;
}

int32_t TraderMocker::match_once()
{
	if (_terminated || _orders == NULL || _orders->size() == 0 || _ticks == NULL)
		return 0;
	
	int32_t count = 0;
	for (const std::string& fullcode : _codes)
	{
		if (_terminated)
			return count;

		std::string code, exchg;
		auto pos = fullcode.find(".");
		exchg = fullcode.substr(0, pos);
		code = fullcode.substr(pos + 1);

		WTSContractInfo* ct = _bd_mgr->getContract(code.c_str(), exchg.c_str());
		if (ct == NULL)
			continue;

		WTSCommodityInfo* commInfo = _bd_mgr->getCommodity(ct);

		WTSTickData* curTick = (WTSTickData*)_ticks->grab(fullcode);
		if (curTick && strcmp(curTick->code(), ct->getCode())==0)
		{
			BoostUniqueLock lock(_mtx_awaits);
			uint64_t tickTime = (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime();
			if (decimal::gt(curTick->price(), 0) && tickTime >= _last_match_time)
			{
				//��ʼ��������
				//������¼
				std::vector<std::string> to_erase;

				for (auto it = _awaits->begin(); it != _awaits->end(); it++)
				{
					WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;
					if (ordInfo->getVolLeft() == 0 || strcmp(ordInfo->getCode(), curTick->code()) != 0)
						continue;

					bool isBuy = (ordInfo->getDirection() == WDT_LONG && ordInfo->getOffsetType() == WOT_OPEN) || (ordInfo->getDirection() != WDT_LONG && ordInfo->getOffsetType() != WOT_OPEN);

					double uPrice, uVolumn;
					if (isBuy)
					{
						uPrice = curTick->askprice(0);
						uVolumn = curTick->askqty(0);
					}
					else
					{
						uPrice = curTick->bidprice(0);
						uVolumn = curTick->bidqty(0);
					}

					if (decimal::eq(uVolumn, 0))
						continue;					

					if (_use_newpx)
					{
						uPrice = curTick->price();
					}

					if (decimal::eq(uPrice, 0))
						continue;

					double target = ordInfo->getPrice();
					//�����ʱ��ί�м۸�С�����¼��򲻳ɽ���������ʱ��ί�м۴������¼��򲻳ɽ�
					if (ordInfo->getPriceType() == WPT_LIMITPRICE && ((isBuy && decimal::lt(target, uPrice)) || (!isBuy && decimal::gt(target, uPrice))))
						continue;

					count++;

					double maxVolumn = min(uVolumn, ordInfo->getVolLeft());
					std::vector<uint32_t> ayVol = splitVolumn((uint32_t)maxVolumn, (uint32_t)_min_qty, (uint32_t)_max_qty);
					for (uint32_t curVol : ayVol)
					{

						WTSTradeInfo* trade = WTSTradeInfo::create(curTick->code(), curTick->exchg());
						trade->setDirection(ordInfo->getDirection());
						trade->setOffsetType(ordInfo->getOffsetType());

						trade->setPrice(uPrice);
						trade->setVolumn(curVol);

						trade->setRefOrder(ordInfo->getOrderID());

						char str[64];
						sprintf(str, "mt.%u.%u", _mocker_id, makeTradeID());
						trade->setTradeID(str);

						trade->setTradeTime(TimeUtils::getLocalTimeNow());

						//���¶�������
						ordInfo->setVolLeft(ordInfo->getVolLeft() - curVol);
						ordInfo->setVolTraded(ordInfo->getVolTraded() - curVol);
						if (decimal::eq(ordInfo->getVolLeft(), 0))
						{
							ordInfo->setOrderState(WOS_AllTraded);
							ordInfo->setStateMsg("ȫ���ɽ�");
							to_erase.push_back(ordInfo->getOrderID());
						}
						else
						{
							ordInfo->setOrderState(WOS_PartTraded_Queuing);
							ordInfo->setStateMsg("���ֳɽ�");
						}

						PosItem& pItem = _positions[ct->getFullCode()];
						//��һ�εĻ�Ҫ������ͽ�������ֵ
						if(strlen(pItem._code) == 0)
						{
							strcpy(pItem._code, ct->getCode());
							strcpy(pItem._exchg, ct->getExchg());
						}

						if(commInfo->getCoverMode() == CM_None)
						{

						}
						else
						{
							if (ordInfo->getDirection() == WDT_LONG)
							{
								if (ordInfo->getOffsetType() == WOT_OPEN)
								{
									pItem._long._volumn += curVol;
								}
								else
								{
									pItem._long._volumn -= curVol;
									pItem._long._frozen -= curVol;
								}
							}
							else
							{
								if (ordInfo->getOffsetType() == WOT_OPEN)
								{
									pItem._short._volumn += curVol;
								}
								else
								{
									pItem._short._volumn -= curVol;
									pItem._short._frozen -= curVol;
								}
							}
						}
						

						if (_listener)
						{
							BoostUniqueLock lock(_mutex_api);
							_listener->onPushOrder(ordInfo);
							_listener->onPushTrade(trade);
						}

						if (_trades == NULL)
							_trades = WTSArray::create();

						_trades->append(trade, false);
					}
				}

				if (count > 0)
				{
					//_listener->handleTraderLog(LL_INFO, "[TraderMocker]���� %s.%s ���� %u ��,�۸�:%u", tick->exchg(), tick->code(), iCount, uPrice);
					for (const std::string& oid : to_erase)
					{
						_awaits->remove(oid);
					}
				}
			}
		}

		if(curTick)
			curTick->release();
	}

	_last_match_time = _max_tick_time;

	if(count > 0)
		save_positions();

	return count;
}

int32_t TraderMocker::procOrdersOpenLong(WTSContractInfo* ct, WTSTickData* tick)
{
	//������,��ȡ��һ�ۺ���һ��
	double uPrice = tick->askprice(0);
	double uVolumn = tick->askqty(0);
	if (decimal::eq(uVolumn,0))
		return 0;

	if (_use_newpx)
	{
		uPrice = tick->price();
	}

	if(decimal::eq(uPrice, 0))
		return 0;

	//������¼
	int32_t iCount = 0;
	std::vector<std::string> to_erase;

	for (auto it = _awaits->begin(); it != _awaits->end(); it++)
	{
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;
		if (ordInfo->getVolLeft() == 0 || strcmp(ordInfo->getCode(), tick->code()) == 0)
			continue;

		iCount++;

		double maxVolumn = min(uVolumn, ordInfo->getVolLeft());
		std::vector<uint32_t> ayVol = splitVolumn((uint32_t)maxVolumn, (uint32_t)_min_qty, (uint32_t)_max_qty);
		for (uint32_t curVol : ayVol)
		{

			WTSTradeInfo* trade = WTSTradeInfo::create(tick->code(), tick->exchg());
			trade->setDirection(ordInfo->getDirection());
			trade->setOffsetType(ordInfo->getOffsetType());

			trade->setPrice(uPrice);
			trade->setVolumn(curVol);
			
			trade->setRefOrder(ordInfo->getOrderID());

			char str[64];
			sprintf(str, "mt.%u.%u", _mocker_id, makeTradeID());
			trade->setTradeID(str);

			trade->setTradeTime(TimeUtils::getLocalTimeNow());

			//���¶�������
			ordInfo->setVolLeft(ordInfo->getVolLeft() - curVol);
			ordInfo->setVolTraded(ordInfo->getVolTraded() - curVol);
			if(decimal::eq(ordInfo->getVolLeft(), 0))
			{
				ordInfo->setOrderState(WOS_AllTraded);
				ordInfo->setStateMsg("ȫ���ɽ�");
				to_erase.push_back(ordInfo->getOrderID());
			}
			else
			{
				ordInfo->setOrderState(WOS_PartTraded_Queuing);
				ordInfo->setStateMsg("���ֳɽ�");
			}

			if(_listener)
			{
				BoostUniqueLock lock(_mutex_api);
				_listener->onPushOrder(ordInfo);
				_listener->onPushTrade(trade);
			}

			if (_trades == NULL)
				_trades = WTSArray::create();

			_trades->append(trade, false);
		}
	}

	if (iCount > 0)
	{
		_listener->handleTraderLog(LL_INFO, "[TraderMocker]���� %s.%s ���� %u ��,�۸�:%u", tick->exchg(), tick->code(), iCount, uPrice);
		for (const std::string& oid : to_erase)
		{
			_awaits->remove(oid);
		}
	}

	return iCount;
}

int32_t TraderMocker::procOrdersCloseShort(WTSContractInfo* ct, WTSTickData* tick)
{
	//������,��ȡ��һ�ۺ���һ��
	double price = tick->askprice(0);
	double volumn = tick->askqty(0);
	if (decimal::eq(volumn,0))
		return 0;

	if (_use_newpx)
	{
		price = tick->price();
	}

	if(decimal::eq(price, 0))
		return 0;

	//������¼
	int32_t iCount = 0;
	std::vector<std::string> to_erase;

	for (auto it = _awaits->begin(); it != _awaits->end(); it++)
	{
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;
		if (ordInfo->getVolLeft() == 0 || strcmp(ordInfo->getCode(), tick->code()) == 0)
			continue;

		iCount++;

		double maxVolumn = min(volumn, ordInfo->getVolLeft());
		std::vector<uint32_t> ayVol = splitVolumn((uint32_t)maxVolumn, (uint32_t)_min_qty, (uint32_t)_max_qty);
		for (uint32_t curVol : ayVol)
		{

			WTSTradeInfo* trade = WTSTradeInfo::create(tick->code(), tick->exchg());
			trade->setDirection(ordInfo->getDirection());
			trade->setOffsetType(ordInfo->getOffsetType());

			trade->setPrice(price);
			trade->setVolumn(curVol);

			trade->setRefOrder(ordInfo->getOrderID());

			char str[64];
			sprintf(str, "mt.%u.%u", _mocker_id, makeTradeID());
			trade->setTradeID(str);

			trade->setTradeTime(TimeUtils::getLocalTimeNow());

			//���¶�������
			ordInfo->setVolLeft(ordInfo->getVolLeft() - curVol);
			ordInfo->setVolTraded(ordInfo->getVolTraded() - curVol);
			if(decimal::eq(ordInfo->getVolLeft(), 0))
			{
				ordInfo->setOrderState(WOS_AllTraded);
				ordInfo->setStateMsg("ȫ���ɽ�");
				to_erase.push_back(ordInfo->getOrderID());
			}
			else
			{
				ordInfo->setOrderState(WOS_PartTraded_Queuing);
				ordInfo->setStateMsg("���ֳɽ�");
			}

			if(_listener)
			{
				BoostUniqueLock lock(_mutex_api);
				_listener->onPushOrder(ordInfo);
				_listener->onPushTrade(trade);
			}

			if (_trades == NULL)
				_trades = WTSArray::create();

			_trades->append(trade, false);
		}
	}

	if (iCount > 0)
	{
		_listener->handleTraderLog(LL_INFO, "���� %s.%s ƽ�� %u ��,�۸�:%u", tick->exchg(), tick->code(), iCount, price);
		for (const std::string& oid : to_erase)
		{
			_awaits->remove(oid);
		}
	}

	return iCount;
}

int32_t TraderMocker::procOrdersOpenShort(WTSContractInfo* ct, WTSTickData* tick)
{
	//������,��ȡ��һ�ۺ���һ��
	double uPrice = tick->bidprice(0);
	double uVolumn = tick->bidqty(0);
	if (decimal::eq(uVolumn,0))
		return 0;

	if(_use_newpx)
	{
		uPrice = tick->price();
	}

	if(decimal::eq(uPrice, 0))
		return 0;

	//������¼
	int32_t iCount = 0;
	std::vector<std::string> to_erase;

	for (auto it = _awaits->begin(); it != _awaits->end(); it++)
	{
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;
		if (ordInfo->getVolLeft() == 0 || strcmp(ordInfo->getCode(), tick->code()) == 0)
			continue;

		iCount++;

		double maxVolumn = min(uVolumn, ordInfo->getVolLeft());
		std::vector<uint32_t> ayVol = splitVolumn((uint32_t)maxVolumn, (uint32_t)_min_qty, (uint32_t)_max_qty);
		for (uint32_t curVol : ayVol)
		{

			WTSTradeInfo* trade = WTSTradeInfo::create(tick->code(), tick->exchg());
			trade->setDirection(ordInfo->getDirection());
			trade->setOffsetType(ordInfo->getOffsetType());

			trade->setPrice(uPrice);
			trade->setVolumn(curVol);

			trade->setRefOrder(ordInfo->getOrderID());

			char str[64];
			sprintf(str, "mt.%u.%u", _mocker_id, makeTradeID());
			trade->setTradeID(str);

			trade->setTradeTime(TimeUtils::getLocalTimeNow());

			//���¶�������
			ordInfo->setVolLeft(ordInfo->getVolLeft() - curVol);
			ordInfo->setVolTraded(ordInfo->getVolTraded() - curVol);
			if(decimal::eq(ordInfo->getVolLeft(), 0))
			{
				ordInfo->setOrderState(WOS_AllTraded);
				ordInfo->setStateMsg("ȫ���ɽ�");
				to_erase.push_back(ordInfo->getOrderID());
			}
			else
			{
				ordInfo->setOrderState(WOS_PartTraded_Queuing);
				ordInfo->setStateMsg("���ֳɽ�");
			}

			if (_listener)
			{
				BoostUniqueLock lock(_mutex_api);
				_listener->onPushOrder(ordInfo);
				_listener->onPushTrade(trade);
			}

			if (_trades == NULL)
				_trades = WTSArray::create();

			_trades->append(trade, false);
		}
	}

	if (iCount > 0)
	{
		_listener->handleTraderLog(LL_INFO, "���� %s.%s ���� %u ��,�۸�:%u", tick->exchg(), tick->code(), iCount, uPrice);
		for (const std::string& oid : to_erase)
		{
			_awaits->remove(oid);
		}
	}

	return iCount;
}

int32_t TraderMocker::procOrdersCloseLong(WTSContractInfo* ct, WTSTickData* tick)
{
	//������,��ȡ��һ�ۺ���һ��
	double uPrice = tick->bidprice(0);
	double uVolumn = tick->bidqty(0);
	if (decimal::eq(uVolumn,0))
		return 0;

	if (_use_newpx)
	{
		uPrice = tick->price();
	}

	if(decimal::eq(uPrice, 0))
		return 0;

	//������¼
	int32_t iCount = 0;

	std::vector<std::string> to_erase;

	for (auto it = _awaits->begin(); it != _awaits->end(); it++)
	{
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;
		if (ordInfo->getVolLeft() == 0 || strcmp(ordInfo->getCode(), tick->code()) == 0)
			continue;

		iCount++;

		double maxVolumn = min(uVolumn, ordInfo->getVolLeft());
		std::vector<uint32_t> ayVol = splitVolumn((uint32_t)maxVolumn, (uint32_t)_min_qty, (uint32_t)_max_qty);
		for (uint32_t curVol : ayVol)
		{

			WTSTradeInfo* trade = WTSTradeInfo::create(tick->code(), tick->exchg());
			trade->setDirection(ordInfo->getDirection());
			trade->setOffsetType(ordInfo->getOffsetType());

			trade->setPrice(uPrice);
			trade->setVolumn(curVol);

			trade->setRefOrder(ordInfo->getOrderID());

			char str[64];
			sprintf(str, "mt.%u.%u", _mocker_id, makeTradeID());
			trade->setTradeID(str);

			trade->setTradeTime(TimeUtils::getLocalTimeNow());

			//���¶�������
			ordInfo->setVolLeft(ordInfo->getVolLeft() - curVol);
			ordInfo->setVolTraded(ordInfo->getVolTraded() - curVol);
			if(decimal::eq(ordInfo->getVolLeft(), 0))
			{
				ordInfo->setOrderState(WOS_AllTraded);
				ordInfo->setStateMsg("ȫ���ɽ�");
				to_erase.push_back(ordInfo->getOrderID());
			}
			else
			{
				ordInfo->setOrderState(WOS_PartTraded_Queuing);
				ordInfo->setStateMsg("���ֳɽ�");
			}

			if (_listener)
			{
				BoostUniqueLock lock(_mutex_api);
				_listener->onPushOrder(ordInfo);
				_listener->onPushTrade(trade);
			}

			if (_trades == NULL)
				_trades = WTSArray::create();

			_trades->append(trade, false);
		}
	}

	if (iCount > 0)
	{
		_listener->handleTraderLog(LL_INFO, "���� %s.%s ƽ�� %u ��,�۸�:%u", tick->exchg(), tick->code(), iCount, uPrice);

		for(const std::string& oid : to_erase)
		{
			_awaits->remove(oid);
		}
	}

	return iCount;
}

//////////////////////////////////////////////////////////////////////////

bool TraderMocker::init(WTSParams *params)
{
	_millisecs = params->getUInt32("span");
	_use_newpx = params->getBoolean("newpx");
	_mocker_id = params->getUInt32("mockerid");
	_max_qty = params->getDouble("maxqty");
	_min_qty = params->getDouble("minqty");

	_udp_port = params->getInt32("udp_port");

	boost::asio::ip::address addr = boost::asio::ip::address::from_string("0.0.0.0");
	_broad_ep = boost::asio::ip::udp::endpoint(addr, _udp_port);

	if (decimal::eq(_max_qty, 0))
		_max_qty = 100;

	if (decimal::eq(_min_qty, 0))
		_min_qty = 1;

	//���سֲ�����
	std::stringstream ss;
	ss << "./mocker_" << _mocker_id << "/";
	std::string path = ss.str();
	BoostFile::create_directories(path.c_str());

	_pos_file = path;
	_pos_file += "positions.json";

	load_positions();

	return true;
}

void TraderMocker::load_positions()
{
	if (!BoostFile::exists(_pos_file.c_str()))
		return;

	std::string json;
	BoostFile::read_file_contents(_pos_file.c_str(), json);

	rj::Document root;
	root.Parse(json.c_str());
	if (root.HasParseError())
		return;

	if(root.HasMember("positions"))
	{//��ȡ��λ
		double total_profit = 0;
		double total_dynprofit = 0;
		const rj::Value& jPos = root["positions"];
		if (!jPos.IsNull() && jPos.IsArray())
		{
			for (const rj::Value& pItem : jPos.GetArray())
			{
				const char* exchg = pItem["exchg"].GetString();
				const char* code = pItem["code"].GetString();
				WTSContractInfo* ct = _bd_mgr->getContract(code, exchg);
				if (ct == NULL)
					continue;

				PosItem& pInfo = _positions[ct->getFullCode()];
				strcpy(pInfo._code, ct->getCode());
				strcpy(pInfo._exchg, ct->getExchg());
				
				pInfo._long._volumn = pItem["long"]["volumn"].GetDouble();

				pInfo._short._volumn = pItem["short"]["volumn"].GetDouble();
			}
		}
	}
}

void TraderMocker::save_positions()
{
	rj::Document root(rj::kObjectType);

	{//�ֲ����ݱ���
		rj::Value jPos(rj::kArrayType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto& v : _positions)
		{
			const char* fullcode = v.first.c_str();
			const PosItem& pInfo = v.second;

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("exchg", rj::Value(pInfo._exchg, allocator), allocator);
			pItem.AddMember("code", rj::Value(pInfo._code, allocator), allocator);

			{
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("volumn", pInfo._long._volumn, allocator);

				pItem.AddMember("long", dItem, allocator);
			}

			{
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("volumn", pInfo._short._volumn, allocator);

				pItem.AddMember("short", dItem, allocator);
			}


			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	{
		BoostFile bf;
		if (bf.create_new_file(_pos_file.c_str()))
		{
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);
			bf.write_file(sb.GetString());
			bf.close_file();
		}
	}
}

void TraderMocker::release()
{
	if (_terminated)
		return;

	_terminated = true;

	if (_thrd_match)
	{
		_thrd_match->join();
	}

	if (_thrd_worker)
	{
		_io_service.stop();
		_thrd_worker->join();
	}
}

void TraderMocker::registerListener(ITraderApiListener *listener)
{
	_listener = listener;

	_bd_mgr = listener->getBaseDataMgr();
}

void TraderMocker::reconn_udp()
{
	if (_b_socket != NULL)
	{
		_b_socket->close();
		delete _b_socket;
		_b_socket = NULL;
	}

	_b_socket = new boost::asio::ip::udp::socket(_io_service);

	_b_socket->open(_broad_ep.protocol());
	_b_socket->set_option(boost::asio::ip::udp::socket::reuse_address(true));
	_b_socket->set_option(boost::asio::ip::udp::socket::broadcast(true));
	_b_socket->bind(_broad_ep);


	_b_socket->async_receive_from(boost::asio::buffer(_b_buffer), _broad_ep,
		boost::bind(&TraderMocker::handle_read, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred, true));
}

void TraderMocker::connect()
{
	reconn_udp();

	_thrd_worker.reset(new BoostThread(boost::bind(&boost::asio::io_service::run, &_io_service)));

	_io_service.post([this](){
		BoostUniqueLock lock(_mutex_api);

		if (_listener)
			_listener->handleEvent(WTE_Connect, 0);
	});
}

void TraderMocker::disconnect()
{
	if (_terminated)
		return;

	_terminated = true;

	if (_thrd_match)
	{
		_thrd_match->join();
	}
}

bool TraderMocker::isConnected()
{
	return _thrd_match != NULL;
}

int TraderMocker::login(const char* user, const char* pass, const char* productInfo)
{
	_thrd_match.reset(new BoostThread([this]() {
		while (!_terminated)
		{
			match_once();

			//�ȴ�5����
			boost::this_thread::sleep(boost::posix_time::millisec(5));
		}
	}));

	_io_service.post([this](){
		BoostUniqueLock lock(_mutex_api);

		if (_listener)
			_listener->onLoginResult(true, "", TimeUtils::getCurDate());
	});

	return 0;
}

int TraderMocker::logout()
{
	return 0;
}

int TraderMocker::orderAction(WTSEntrustAction* action)
{
	action->retain();
	
	_io_service.post([this, action](){
		BoostUniqueLock lck(_mtx_awaits);	//һ��Ҫ��awaits����������Ȼ���ܻᵼ��һ�ߴ��һ�߳���
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)_awaits->grab(action->getOrderID());
		WTSContractInfo* ct = _bd_mgr->getContract(ordInfo->getCode(), ordInfo->getExchg());
		WTSCommodityInfo* commInfo = _bd_mgr->getCommodity(ct);
		/*
		 *	����ҲҪ���Ǽ�������
		 *	1���Ƿ��ڿ��Գ�����״̬
		 *	2������ǿ��֣���ֱ�ӳ���
		 *	3�������ƽ�֣�Ҫ�ͷŶ���
		 */
		if(ordInfo == NULL)
		{
			WTSError* err = WTSError::create(WEC_ORDERCANCEL, "���������ڻ��ߴ��ڲ��ɳ���״̬");
			if (_listener)
				_listener->onTraderError(err);
			err->release();
			return;
		}

		bool bPass = false;
		do 
		{
			//����ί��ֱ�ӳ���
			if (ordInfo->getOffsetType() == WOT_OPEN)
			{
				bPass = true;
				break;
			}
			
			//�����ֿ�ƽ�ģ�Ҳֱ�ӳ���
			if (commInfo->getCoverMode() == CM_None)
			{
				bPass = true;
				break;
			}

			//�ͷŶ���ֲ�
			PosItem& pItem = _positions[ct->getFullCode()];
			bool isLong = ordInfo->getDirection() == WDT_LONG;
			if(isLong)
			{
				pItem._long._frozen -= ordInfo->getVolLeft();
			}
			else
			{
				pItem._short._frozen -= ordInfo->getVolLeft();
			}
			bPass = true;

		} while (false);

		ordInfo->setStateMsg("�����ɹ�");
		ordInfo->setOrderState(WOS_Canceled);
		ordInfo->setVolLeft(0);	

		if (_listener)
		{
			BoostUniqueLock lock(_mutex_api);
			_listener->onPushOrder(ordInfo);
		}

		ordInfo->release();
		action->release();

		save_positions();
	});

	return 0;
}

int TraderMocker::queryAccount()
{
	_io_service.post([this](){
		WTSArray* ay = WTSArray::create();
		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setCurrency("CNY");
		accountInfo->setBalance(0);
		accountInfo->setPreBalance(0);
		accountInfo->setCloseProfit(0);
		accountInfo->setMargin(0);
		accountInfo->setAvailable(0);
		accountInfo->setCommission(0);
		accountInfo->setFrozenMargin(0);
		accountInfo->setFrozenCommission(0);
		accountInfo->setDeposit(0);
		accountInfo->setWithdraw(0);
		accountInfo->setDescription("");
		accountInfo->setDynProfit(0);

		ay->append(accountInfo, false);

		if (_listener)
		{
			BoostUniqueLock lock(_mutex_api);
			_listener->onRspAccount(ay);
		}

		ay->release();

	});
	return 0;
}

int TraderMocker::queryPositions()
{
	_io_service.post([this](){
		WTSArray* ayPos = WTSArray::create();

		for(auto& v : _positions)
		{
			const PosItem& pItem = v.second;

			WTSContractInfo* ct = _bd_mgr->getContract(pItem._code, pItem._exchg);
			if(ct == NULL)
				continue;

			WTSCommodityInfo* commInfo = _bd_mgr->getCommodity(ct);

			if(pItem._long._volumn > 0)
			{
				WTSPositionItem* pInfo = WTSPositionItem::create(pItem._code, commInfo->getCurrency(), pItem._exchg);
				pInfo->setDirection(WDT_LONG);
				pInfo->setNewPosition(pItem._long._volumn);
				pInfo->setAvailNewPos(pItem._long._volumn - pItem._long._frozen);

				ayPos->append(pInfo, false);
			}

			if (pItem._short._volumn > 0)
			{
				WTSPositionItem* pInfo = WTSPositionItem::create(pItem._code, commInfo->getCurrency(), pItem._exchg);
				pInfo->setDirection(WDT_SHORT);
				pInfo->setNewPosition(pItem._short._volumn);
				pInfo->setAvailNewPos(pItem._short._volumn - pItem._short._frozen);

				ayPos->append(pInfo, false);
			}
		}

		if (_listener)
		{
			BoostUniqueLock lock(_mutex_api);
			_listener->onRspPosition(ayPos);
		}
		ayPos->release();
	});

	return 0;
}

int TraderMocker::queryOrders()
{
	_io_service.post([this](){
		BoostUniqueLock lock(_mutex_api);

		if (_listener)
			_listener->onRspOrders(_orders);
	});

	return 0;
}

int TraderMocker::queryTrades()
{
	_io_service.post([this](){
		BoostUniqueLock lock(_mutex_api);

		if (_listener)
			_listener->onRspTrades(_trades);
	});

	return 0;
}

void TraderMocker::handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad /* = true */)
{
	if (e)
	{
		if (_listener)
			_listener->handleTraderLog(LL_ERROR, "[TraderMocker]UDP������ճ���:%s(%d)", e.message().c_str(), e.value());

		if (!_terminated)
		{
			boost::this_thread::sleep(boost::posix_time::seconds(2));
			reconn_udp();
			return;
		}
	}

	if (_terminated || bytes_transferred <= 0)
		return;

	extract_buffer(bytes_transferred, isBroad);

	if (isBroad && _b_socket)
	{
		_b_socket->async_receive_from(boost::asio::buffer(_b_buffer), _broad_ep,
			boost::bind(&TraderMocker::handle_read, this,
			boost::asio::placeholders::error,
			boost::asio::placeholders::bytes_transferred, true));
	}
}

#define UDP_MSG_PUSHTICK	0x200
#pragma pack(push,1)

typedef struct UDPPacketHead
{
	uint32_t		_type;
} UDPPacketHead;
//UDP�����
typedef struct _UDPReqPacket : UDPPacketHead
{
	char			_data[1020];
} UDPReqPacket;

//UDPTick���ݰ�
template <typename T>
struct UDPDataPacket : UDPPacketHead
{
	T			_data;
};
#pragma pack(pop)
typedef UDPDataPacket<WTSTickStruct>	UDPTickPacket;
void TraderMocker::extract_buffer(uint32_t length, bool isBroad /* = true */)
{
	UDPPacketHead* header = (UDPTickPacket*)_b_buffer.data();

	if (header->_type == UDP_MSG_PUSHTICK)
	{
		UDPTickPacket* packet = (UDPTickPacket*)header;
		std::string fullcode = StrUtil::printf("%s.%s", packet->_data.exchg, packet->_data.code);
		auto it = _codes.find(fullcode);
		if (it == _codes.end())
			return;

		WTSTickData* curTick = WTSTickData::create(packet->_data);
		
		if (_ticks == NULL)
			_ticks = TickCache::create();

		_ticks->add(fullcode, curTick, false);

		_max_tick_time = max(_max_tick_time, (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime());
	}
}