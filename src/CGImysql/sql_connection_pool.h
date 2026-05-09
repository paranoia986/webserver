#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

struct PoolStats
{
    int cur_conn;   // 当前已使用的连接数
    int free_conn;  // 当前空闲的连接数
    int max_conn;   // 最大连接数
};

class connection_pool
{
public:
	string m_url;			 //主机地址
	string m_Port;		 //数据库端口号
	string m_Sock;
	string m_User;		 //登陆数据库用户名
	string m_PassWord;	 //登陆数据库密码
	string m_DatabaseName; //使用数据库名
	int m_close_log;	//日志开关

private:
	int m_MaxConn;  //最大连接数
	int m_CurConn;  //当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	locker lock;
	list<MYSQL *> connList; //连接池
	sem reserve;

public:
	MYSQL *GetConnection();				 //获取数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接
	int GetFreeConn();					 //获取连接
	PoolStats GetStats();                //获取连接池统计
	void DestroyPool();					 //销毁所有连接
	//单例模式
	static connection_pool *GetInstance();
	// TCP/IP 连接
	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log); 
	// Unix Domain Socket 连接
	void init(string sock, string User, string PassWord, string DataBaseName, int MaxConn, int close_log);

private:
	connection_pool();
	~connection_pool();
};

class connectionRAII{
private:
	MYSQL *conRAII;
	connection_pool *poolRAII;

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);
	~connectionRAII();
};

#endif
