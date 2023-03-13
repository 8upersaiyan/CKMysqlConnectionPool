#include "pch.h"
#include "CommonConnectionPool.h"
#include "public.h"
#include <iostream>

// �̰߳�ȫ���������������ӿ� 
ConnectionPool* ConnectionPool::getConnectionPool()
{
	static ConnectionPool pool; // lock��unlock ��̬�ĳ�ʼ����һ�����вŻ��ʼ�� �̰߳�ȫ
	return &pool;
}

// �������ļ��м���������
bool ConnectionPool::loadConfigFile()
{
	FILE* pf = fopen("mysql.ini", "r");
	if (pf == nullptr)
	{
		LOG("mysql.ini file is not exist!"); //��־��Ϣ
		return false;
	}

	while (!feof(pf)) //feof ���ڲ�ѯ�ļ�ָ���Ƿ��Դﵽĩβ
	{
		char line[1024] = { 0 };
		fgets(line, 1024, pf);
		string str = line;

		//�ҵȺ�
		int idx = str.find('=', 0);
		if (idx == -1) {	//��Ч������
			continue;
		}

		//�һس�
		//password=1234\n
		int endidx = str.find('\n', idx);
		//�����ı�����
		string key = str.substr(0, idx);
		string value = str.substr(idx + 1, endidx - idx - 1);

		if (key == "ip")
		{
			_ip = value;
		}
		else if (key == "port")
		{
			_port = atoi(value.c_str());
		}
		else if (key == "username")
		{
			_username = value;
		}
		else if (key == "password")
		{
			_password = value;
		}
		else if (key == "dbname")
		{
			_dbname = value;
		}
		else if (key == "initSize")
		{
			_initSize = atoi(value.c_str());
		}
		else if (key == "maxSize")
		{
			_maxSize = atoi(value.c_str());
		}
		else if (key == "maxIdleTime")
		{
			_maxIdleTime = atoi(value.c_str());
		}
		else if (key == "connectionTimeOut")
		{
			_connectionTimeout = atoi(value.c_str());
		}
	}
	return true;
}

//���ӳصĹ��캯��
ConnectionPool::ConnectionPool() {

	// ������������
	if (!loadConfigFile())
	{
		return;
	}
	// ������ʼ����������
	for (int i = 0; i < _initSize; ++i)
	{
		Connection* p = new Connection();
		p->connect(_ip, _port, _username, _password, _dbname);
		p->refreshAliveTime(); // ˢ��һ�¿�ʼ���е���ʼʱ��
		_connectionQue.push(p);
		_connectionCnt++;
	}

	// ����
	// ����һ���µ��̣߳���Ϊ���ӵ������� linux thread => pthread_create 
	thread produce(std::bind(&ConnectionPool::produceConnectionTask, this));
	produce.detach();//�����߳�

	// ����һ���µĶ�ʱ�̣߳�ɨ�賬��maxIdleTimeʱ��Ŀ������ӣ����ж��ڵ����ӻ���
	thread scanner(std::bind(&ConnectionPool::scannerConnectionTask, this));
	scanner.detach();//�����߳�
}

//�����ڶ������߳��У�ר�Ÿ�������������     �������߳�
void ConnectionPool::produceConnectionTask() {
	for (;;) {
		unique_lock<mutex> lock(_queueMutex);
		while (!_connectionQue.empty()) {
			cv.wait(lock); //���в�Ϊ�գ� �˴������߳̽���ȴ�״̬
		}

		// ��������û�е������ޣ����������µ�����
		if (_connectionCnt < _maxSize) {
			Connection* p = new Connection();
			p->connect(_ip, _port, _username, _password, _dbname);
			p->refreshAliveTime(); // ˢ��һ�¿�ʼ���е���ʼʱ��
			_connectionQue.push(p);
			_connectionCnt++;  //��������++
		}
		// ֪ͨ�������̣߳� ��������������
		cv.notify_all();
	}
}

//���ⲿ�ṩ�ӿڣ������ӳ��л�ȡһ�����õĿ�������
shared_ptr<Connection> ConnectionPool::getConnection() {

	unique_lock<mutex> lock(_queueMutex);
	//����Ϊ��
	while (_connectionQue.empty()) {
		//��ʱ
		if (cv_status::timeout == cv.wait_for(lock, chrono::milliseconds(_connectionTimeout)))
		{
			if (_connectionQue.empty()) //��ʱ�������ֻ��ǿյ� 
			{
				LOG("��ȡ�ռ����ӳ�ʱ�ˡ�������ȡ����ʧ�ܣ�");
				return nullptr;
			}
		}
	}
	/*
	* share_ptr����ָ������ʱ�����connection��Դֱ��delete�����൱�ڵ���connection������������connection�ͱ�close����
	* ������Ҫ�Զ���shared_ptr���ͷ���Դ�ķ�ʽ����connectionֱ�ӹ黹��queue��
	*/
	//���в�Ϊ�� ----��������
	shared_ptr<Connection> sp(_connectionQue.front(), 
		[&](Connection *pcon) 
		{
			// �������ڷ�����Ӧ���߳��е��õģ�����һ��Ҫ���Ƕ��е��̰߳�ȫ����
			unique_lock<mutex> lock(_queueMutex);
			pcon->refreshAliveTime(); // ˢ��һ�¿�ʼ���е���ʼʱ��
			_connectionQue.push(pcon);
	    });

	_connectionQue.pop(); //���ѵ�ͷ��

	if (_connectionQue.empty()) //���ѵ�ͷ���� ���б�Ϊ�� 
	{
		//˭�����˶��������һ��connection��˭����֪ͨһ����������
		cv.notify_all(); 
	}
	return sp;
}

//ɨ�賬��maxIdleTimeʱ��Ŀ������ӣ����ж��ڵ����ӻ���
void ConnectionPool::scannerConnectionTask(){
	for (;;) {
		//ͨ�� sleepģ�ⶨʱЧ��
		this_thread::sleep_for(chrono::seconds(_maxIdleTime));
		//ɨ���������У��ͷŶ��������
		unique_lock<mutex> lock(_queueMutex);
		while (_connectionCnt > _initSize) {
			Connection* p = _connectionQue.front(); //ָ���ͷ
			if (p->getAliveeTime() >= (_maxIdleTime * 100))
			{
				_connectionQue.pop();
				_connectionCnt--;
				delete p;
			}
			else
			{
				break; //��ͷ������û�г��� _maxIdleTim �������ӿ϶�û��
			}

		} 
	}
}