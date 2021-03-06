//--------------------------Q-IOT Platform------------------------------------------------------//
/*
Q-Wifi是酷享物联平台的固件实现，基于ESP8266开发，构建了一套与酷享APP、酷享云平台相互配合的物联网
底层实现机制，代码基于ESP8266_RTOS_SDK开发，开源，欢迎更多程序员大牛加入！

Q-Wifi构架了一套设备、变量、情景、动作、用户终端的管理机制，内建了若干个TCP连接与APP及云平台互动，
其中json上报连接，采用通用的http json格式，可被开发者导向至自己的物联网平台，进行数据汇集和控制。

Q-Wifi内建了一套动态的web网页服务器，用来进行硬件配置，此web网页服务器封装完整，可被开发者移植到
其他项目。

Q-Wifi内建了一套基于串口的指令解析系统，用来进行串口调试，指令下发，此解析系统封装完整，包含一系
列解析函数，可被开发者移植至其他项目。

Q-Wifi内部带user标识的文件，均为支持开发者自主修改的客制化文件，特别是user_hook.c文件，系统内所
有关键点都会有hook函数在此文件中，供开发者二次开发。

Q-Wifi代码拥有众多模块化的机制或方法，可以被复用及移植，减少物联网系统的开发难度。
所有基于酷享物联平台进行的开发或案例、产品，均可联系酷享团队，免费放置于酷物联视频（q-iot.cn）进行
传播或有偿售卖，相应所有扣除税费及维护费用后，均全额提供给贡献者，以此鼓励国内开源事业。

By Karlno 酷享科技

本文件定义了一个标准的http json上报机制
*/
//---------------------------------------------------------------------------------------------//
#include "SysDefines.h"
#include "JsonClient.h"
#include "VarsJsonUpTask.h"
#include "AppClientManger.h"

#define JSON_UP_QUEUE_LEN 30

typedef struct{
	JSON_EVENT_NAME Event;

	u8 *pPath;
	u8 *pJsonStr;
	bool PathStrNeedFree:1;
	bool JsonStrNeedFree:1;

	pStdFunc pJsonReturn;

}JSON_UP_EVENT;

static OS_QUEUE_T gJsonUpQueue=NULL;//发包队列

static u32 gJsonQNum=0;

//POST数据组合函数
bool SendPostRequest(const u8 *pUrl,const u8 *pJsonStr)
{
	u16 JsonStrLen=strlen(pJsonStr);
	u8 *pPostStr=Q_Zalloc(JsonStrLen+256);
	u8 *pHost=Q_Zalloc(32);
	u8 *p=NULL;
	u8 *pDir=NULL;
	
	QDB_GetStr(SDN_SYS,SIN_JsonServerURL,pHost);//读取服务器地址
	p=strchr(pHost,':');//取host
	pDir=strchr(pHost,'/');//取路径
	
	if(p) {*p='\0';}
	if(pDir) {*pDir='\0';pDir++;}
	
	if(pDir) sprintf(pPostStr,"POST /%s%s HTTP/1.1\r\n",pDir,pUrl);
	else sprintf(pPostStr,"POST %s HTTP/1.1\r\n",pUrl);
	sprintf(&pPostStr[strlen(pPostStr)],"Host: %s\r\n",pHost);
	sprintf(&pPostStr[strlen(pPostStr)],"Connection: close\r\n");
	sprintf(&pPostStr[strlen(pPostStr)],"Content-Type: application/json; charset=utf-8\r\n");
	sprintf(&pPostStr[strlen(pPostStr)],"Content-Length: %u\r\n\r\n",JsonStrLen);
	sprintf(&pPostStr[strlen(pPostStr)],"%s",pJsonStr);

	//if(NeedDebug(DFT_SRV_JSON)) Debug("SendJson[%u]:%s[END]\r\n",JsonStrLen,pJsonStr);
	if(NeedDebug(DFT_SRV_JSON)) Debug(pPostStr);
	//if(NeedDebug(DFT_SRV_JSON)) DisplayBuf(pPostStr,128,16);
	if(NeedDebug(DFT_SRV_JSON)) Debug("[%u]END\n\r",strlen(pPostStr));

	SendToJsonSrv(pPostStr,strlen(pPostStr));

	Q_Free(pPostStr);
	Q_Free(pHost);
}

//回复报文解析
//返回状态码和json字符串地址
u16 HttpRespParse(const char *pData,char **pJsonStr,u16 *pJsonLen)
{
	char *p1=NULL;
	char *p2=NULL;
	u16 RespCode=0;

	if(pData==NULL) return 800;
	
	//先找状态码
	p1=strstr(pData,"HTTP");
	if(p1==NULL) p1=strstr(pData,"http");
	if(p1==NULL) return 801;
	p2=strchr(p1,' ');
	if(p2==NULL) return 802;

	while(*p2==' ') p2++;
	while(*p2)//转换返回码
	{
		if(*p2<'0'||*p2>'9') break;//str不正确字符
		else RespCode=RespCode*10+(*p2-'0');
		p2++;
	}

	if(RespCode==200)//状态码正确
	{
		u16 JsonLen=0;
		p1=strstr(pData,"Content-Length:");//获取内容长度
		if(p1==NULL) p1=strstr(pData,"content-length:");
		if(p1==NULL) return 803;

		p1=strchr(p1,':');
		p2=strstr(p1,"\r\n");
		if(p2==NULL) return 804;
		JsonLen=Str2Uint(&((char *)p1)[1]);//读取content长度
		if(JsonLen==0) return 805;

		//找到存储字符串的地方
		p1=strstr(p2,"\r\n\r\n{");
		if(p1==NULL) return 806;
		p2=strchr(p1,'{');
		p1=strrchr(p2,'}');
		if(p1==NULL) return 807;
		//Debug("p2=%s,%u,%u-%u\n\r",p2,JsonLen,(u32)p2,(u32)p1);
		if(JsonLen!=((u32)p1-(u32)p2+1)) return 808;//不做分包了，所以长度必须相等		

		if(pJsonStr) *pJsonStr=p2;
		if(pJsonLen) *pJsonLen=JsonLen;
	}
	
	return RespCode;
}

//发包队列处理函数
//流程:接收队列-建立连接-组包-发送-收取回包处理-关闭连接
static void JsonConnTask_WaitEvent(IP_ADDR *pServerIp,u16 ServerPort)
{
	JSON_UP_EVENT JsonEvtItem;
				
	while(1)
	{
		JsonEvtItem.Event=0;
		OS_QueueReceive(gJsonUpQueue,&JsonEvtItem,OS_MAX_DELAY);
		gJsonQNum--;
		
		switch(JsonEvtItem.Event)
		{
			case JEN_UP://上报信息
				if(SysVars()->StopDataUp)
				{
					if(JsonEvtItem.PathStrNeedFree) Q_Free(JsonEvtItem.pPath);//清理内存
					if(JsonEvtItem.JsonStrNeedFree) Q_Free(JsonEvtItem.pJsonStr);//清理内存
				}
				else
				{
					NET_CONN_T *SrvConn;
					NET_BUF_T *NetBuf;
					NET_ERR_T Error;

					SrvConn=netconn_new(NETCONN_TCP);      //创建TCP连接   
					netconn_bind(SrvConn,IP_ADDR_ANY,20000+Rand(0x1ff));           //绑定本地地址和监听的端口号 
					netconn_set_sendtimeout(SrvConn,3*1000);//发送超时设定
					netconn_set_recvtimeout(SrvConn,3*1000);//设置超时掉线			
					Error=netconn_connect(SrvConn,pServerIp,ServerPort);            //连接主机 cnn，目标机IP，端口号  
	    			if(Error==ERR_OK)  //连接成功
					{
						if(NeedDebug(DFT_SRV_JSON)) Debug("Json connect ok!%x\n\r",SrvConn);
						SysVars()->JsonConnStaus=SCS_ONLINE;
						SysVars()->JsonConnPoint=SrvConn;//记录服务器句柄

						SendPostRequest(JsonEvtItem.pPath,JsonEvtItem.pJsonStr);//发出json字符串
						if(JsonEvtItem.PathStrNeedFree) Q_Free(JsonEvtItem.pPath);//清理内存
						if(JsonEvtItem.JsonStrNeedFree) Q_Free(JsonEvtItem.pJsonStr);//清理内存

						//等待回复报文
						while((Error = netconn_recv(SrvConn, &NetBuf)) == ERR_OK) //阻塞此处，开始接收信息
						{
							if(NetBuf->p->tot_len)
							{
								char *pData=Q_Zalloc(2000);//多申请一点，防止溢出
								char *p=NULL;
								u16 RecvLen=0;
								u16 Len,JsonLen;
								u16 RespCode;
								
								do //读取数据
								{
								     netbuf_data(NetBuf, (void *)&p, &Len);//获取指针和长度
								     if(RecvLen+Len<2000)
								     {
									     MemCpy(&pData[RecvLen],p,Len);//直接拷贝内容
									     RecvLen+=Len;
								     }
								}while(netbuf_next(NetBuf) >= 0);
								netbuf_delete(NetBuf);
								pData[RecvLen]='\0';

								if(NeedDebug(DFT_SRV_JSON)) Debug("\n\rJRecv[%u]\n\r",RecvLen);
								//Debug(pData);
								//DisplayBuf(pData,RecvLen,16);

								RespCode=HttpRespParse(pData,&p,&JsonLen);//报文解析

								if(NeedDebug(DFT_SRV_JSON)) Debug("Code[%u]\r\n",RespCode);
								if(RespCode!=200)
								{
									if(NeedDebug(DFT_SRV_JSON)) Debug("Resp %u, Stop Post!\n\r",RespCode);
									SysVars()->StopDataUp=TRUE;//停止上报
								}
								if(JsonLen && NeedDebug(DFT_SRV_JSON)) DisplayStrN(p,JsonLen);
								JsonEvtItem.pJsonReturn(JsonLen,p);
								Q_Free(pData);
 							}
						}

						//连接关闭后就会退出while到这里，超时也会
					}
					else //连接未成功
					{
						if(NeedDebug(DFT_SRV_JSON)) Debug("Json_%x conn err \"%s\"\n\r",SrvConn,lwip_strerr(Error));
						if(JsonEvtItem.PathStrNeedFree) Q_Free(JsonEvtItem.pPath);//清理内存
						if(JsonEvtItem.JsonStrNeedFree) Q_Free(JsonEvtItem.pJsonStr);//清理内存
						
						while(OS_QueueReceive(gJsonUpQueue,&JsonEvtItem,0))//连接错误，将队列清空，避免队列内容过多溢出
						{
							gJsonQNum--;
							if(NeedDebug(DFT_SRV_JSON)) Debug("-JsonQ%u\n\r",gJsonQNum);
							if(JsonEvtItem.PathStrNeedFree) Q_Free(JsonEvtItem.pPath);//清理内存
							if(JsonEvtItem.JsonStrNeedFree) Q_Free(JsonEvtItem.pJsonStr);//清理内存
						}
					}

					if(NeedDebug(DFT_SRV_JSON)) Debug("JsonNeedDel %x %x\n\r",SrvConn,SrvConn->pcb.tcp);
					netconn_close(SrvConn);//虽然对方关了，我们主动再关一次
					netconn_delete(SrvConn);

					SysVars()->JsonConnStaus=SCS_OFFLINE;
					SysVars()->JsonConnPoint=NULL;
				}
				break;
		}  
	}
}

//初始化连接服务器，开启队列监听循环
static void JsonConnTask(void *arg)  
{  
   	NET_ERR_T Error;		
    IP_ADDR ServerIp;           //目标机IP  
    u16 ServerPort=0;       //目标机端口号  

	SysVars()->JsonConnStaus=SCS_OFFLINE;
	SysVars()->JsonConnPoint=NULL;

	if(SysVars()->FinishRtcSync==FALSE) OS_TaskDelayMs(1000); 	 //等待主服务器登陆完成 

	//通过DNS找寻服务器ip
  	{
		char *pURL=Q_Zalloc(LONG_STR_BYTES);
		char *pPort=NULL;
		char *p=NULL;
		
		QDB_GetStr(SDN_SYS,SIN_JsonServerURL,pURL);//读取服务器地址

		pPort=strchr(pURL,':');//查找端口号
		if(pPort){*pPort++=0;ServerPort=Str2Uint(pPort);}	
		if(ServerPort==0) ServerPort=80;

		p=strchr(pURL,'/');
		if(p) *p='\0';//去除目录
		if(NeedDebug(DFT_SRV_JSON)) Debug("Find Json Srv:%s Port:%u\n\r",pURL,ServerPort);
		Error=netconn_gethostbyname((void *)pURL,(void *)&ServerIp);
		switch(Error)
		{
			case ERR_OK://无需寻找
				MemCpy(SysVars()->JsonSrvIp,&ServerIp,sizeof(SysVars()->JsonSrvIp));
				if(NeedDebug(DFT_SRV_JSON)) Debug("Json Srv Ip:%u.%u.%u.%u\n\r",DipIpAddr(ServerIp.addr));
				SysVars()->JsonConnStaus=SCS_FOUND_SRV;
				break;			

			case ERR_VAL://dns服务器返回有误
			case ERR_ARG://dns client错误，域名有误
			case ERR_MEM://内存错误
			default:
				Debug("json dns Error!%d\n\r",Error);
				goto Restart;
		}
		Q_Free(pURL);
	}

	JsonConnSendBeat();
    sys_thread_new( "JsonUp Task",VarsJsonUp_Task,NULL,400,TASK_VAR_JSON_UP_PRIO); 
	JsonConnTask_WaitEvent(&ServerIp,ServerPort);//等待其他线程发json请求	
	
	SysVars()->JsonConnStaus=SCS_OFFLINE;
	SysVars()->JsonConnPoint=NULL;

Restart:
	OS_TaskDelayMs(SRV_FAILD_RETRY_MS); //延时等待重试	
	if(SysVars()->SrvSecretKey) sys_thread_new("JsonConn   ",JsonConnTask,NULL,400,TASK_TCP_CLIENT_PRIO);//重新建立线程，如果由于登陆回包不正确，则SysVars()->SrvSecretKey会被板卡设置为0，则停止连接
	OS_TaskDelete(NULL);//删除线程自己    
}  

//初始化连接
void JsonClient_Init(void)
{
	static bool OnlyFlag=TRUE;

	if(OnlyFlag)
	{
		gJsonUpQueue=OS_QueueCreate(JSON_UP_QUEUE_LEN,sizeof(JSON_UP_EVENT));//第一个参数指定队列深度，第二个参数指定成员大小
		QHeapMemSetName(gJsonUpQueue,"JsonUpQ");
		sys_thread_new("JsonConn   ",JsonConnTask,NULL,400,TASK_TCP_CLIENT_PRIO);
		OnlyFlag=FALSE;
	}	
}

//删除链接
void DeleteJsonConn(void)
{
	if(SysVars()->JsonConnPoint)
	{	
		if(NeedDebug(DFT_SRV_JSON)) Debug("Del Json %x\n\r",SysVars()->JsonConnPoint);
		netconn_close(SysVars()->JsonConnPoint);
		netconn_delete(SysVars()->JsonConnPoint);
		SysVars()->JsonConnStaus=SCS_OFFLINE;
		SysVars()->JsonConnPoint=NULL;
	}
}

//供应用程序调用的，上报数据给json服务器的函数，异步
void UpJsonDataToSrv(const u8 *pPath,u8 *pJsonStr,pStdFunc JsonSentCb)
{
	JSON_UP_EVENT JsonItem;

	if(gJsonUpQueue==NULL) return;
	if(gJsonQNum>=(JSON_UP_QUEUE_LEN>>1)) return;
	
	JsonItem.Event=JEN_UP;
	JsonItem.pPath=Q_ZallocAsyn(strlen(pPath)+LONG_STR_BYTES+2);//等待json发送完成后释放
	strcpy(JsonItem.pPath,pPath);
	JsonItem.PathStrNeedFree=TRUE;
	JsonItem.pJsonStr=Q_ZallocAsyn(strlen(pJsonStr)+32);//等待json发送完成后释放
	strcpy(JsonItem.pJsonStr,pJsonStr);
	JsonItem.JsonStrNeedFree=TRUE;
	JsonItem.pJsonReturn=JsonSentCb;
	
	if(OS_QueueSend(gJsonUpQueue,&JsonItem,0)!= pdPASS)
	{
		Debug("QSend Failed\n\r");
		if(JsonItem.PathStrNeedFree) Q_Free(JsonItem.pPath);
		if(JsonItem.JsonStrNeedFree) Q_Free(JsonItem.pJsonStr);
	}
	else
	{
		gJsonQNum++;
	}
	if(NeedDebug(DFT_SRV_JSON)) Debug("JsonQ %u\n\r",gJsonQNum);
}


