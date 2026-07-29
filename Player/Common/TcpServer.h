#pragma once

#include <QObject>

class TcpServer  : public QObject
{
	Q_OBJECT

public:
	TcpServer(QObject *parent);
	~TcpServer();
};
