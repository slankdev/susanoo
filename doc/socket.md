
# socketシステムコールについて

現在未実装
dpdkクラスのインスタンスを持ってきて、そこから
所定のsocketに関わるクラスに関する構造体をもらってから
そこに対してread/writeなどを行えるようにすれば
socketオブジェクトは簡単に実装できそう

様々なsocketに対する統一したインターフェースの操作は
socket_implクラスを継承して操作を行う

```
class socket_impl {
	void read();
	void write();
	void ioctl();
	void close();
	void open();
};

class socket_af_link : public socket_impl {};
class socket_af_inet : public socket_impl {};
class socket_af_arp  : public socket_impl {};
```


## 各ソケットに対する参照方法



## Ethernetに関するsocket

AF_LINKのソケットを確保してそれに対して操作を行う。


```
stcp_ifreq ifr;
ifr.if_ifindex = 0;

socket sock(AF_LINK, STCP_TBD, STCP_TBD);
sock.ioctl(SIOC_BINDTODEVICE, &ifr);

sock.ioctl(...);
sock.write(...);
sock.read(...);
```

### MACアドレスの参照

```
dpdk& dpdk = dpdk::instance();
struct stcp_ifreq ifr;

dpdk.devices[0].ioctl(STCP_SIOCGIFHWADDR, &ifr);
printf("addr: %s \n", hw_sockaddr_to_str(&ifr.if_hwaddr));
```



### MACアドレスの設定

```
dpdk& dpdk = dpdk::instance();
struct stcp_ifreq ifr;

memset(&ifr, 0, sizeof ifr);
ifr.if_hwaddr.sa_data[0] = 0x00;
ifr.if_hwaddr.sa_data[1] = 0x11;
ifr.if_hwaddr.sa_data[2] = 0x22;
ifr.if_hwaddr.sa_data[3] = 0x33;
ifr.if_hwaddr.sa_data[4] = 0x44;
ifr.if_hwaddr.sa_data[5] = 0x55;
dpdk.devices[0].ioctl(STCP_SIOCSIFHWADDR, &ifr);
```


### パケットの送信

```
dpdk& dpdk = core::instance().dpdk;

uint8_t buf[] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xee, 0xee,
	0xee, 0xee, 0xee, 0xee, 0x08, 0x06, 0x00, 0x01,
	0x08, 0x00, 0x06, 0x04, 0x00, 0x01, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xc0, 0xa8, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8,
	0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
dpdk.devices[0].write(buf, sizeof(buf));
```


## IPに関するsocket

### IPアドレスの参照

```
dpdk& dpdk = dpdk::instance();
struct stcp_ifreq ifr;

dpdk.devices[0].ioctl(STCP_SIOCGIFADDR, &ifr);
printf("addr: %s \n", p_sockaddr_to_str(&ifr.if_addr));
```

### IPアドレスの設定

```
dpdk& dpdk = dpdk::instance();
struct stcp_sockaddr_in* sin;
struct stcp_ifreq ifr;

memset(&ifr, 0, sizeof ifr);
sin = reinterpret_cast<stcp_sockaddr_in*>(&ifr.if_addr);
sin->sin_addr = stcp_inet_addr(192, 168, 222, 10);
dpdk.devices[0].ioctl(STCP_SIOCSIFADDR, &ifr);
```

## ARPに関するsocket

### ARPテーブルの参照

```
core& stcp = core::instance();
std::vector<stcp_arpreq>* tbl;

stcp.arp.ioctl(STCP_SIOCGARPENT, &tbl);
for (size_t i=0; i<tbl->size(); i++) {
	printf("%zd: %s %s %d\n", i, 
			p_sockaddr_to_str(&(*tbl)[i].arp_pa), 
			hw_sockaddr_to_str(&(*tbl)[i].arp_ha), 
			(*tbl)[i].arp_ifindex);
}
```

### ARPテーブルに対するレコードの追加

```
core& stcp = core::instance();
struct stcp_sockaddr_in* sin;
struct stcp_arpreq req;

req.arp_ifindex = 0;
req.arp_ha.sa_data[0] = 0xee;
req.arp_ha.sa_data[1] = 0xee;
req.arp_ha.sa_data[2] = 0xee;
req.arp_ha.sa_data[3] = 0xee;
req.arp_ha.sa_data[4] = 0xee;
req.arp_ha.sa_data[5] = 0xee;
sin = reinterpret_cast<stcp_sockaddr_in*>(&req.arp_pa);
sin->sin_addr = stcp_inet_addr(192, 168, 222, 111);
stcp.arp.ioctl(STCP_SIOCAARPENT, &req);
```

### ARPテーブルに対するレコードの消去

```
core& stcp = core::instance();
struct stcp_sockaddr_in* sin;
struct stcp_arpreq req;

req.arp_ifindex = 0;
sin->sin_addr = stcp_inet_addr(192, 168, 222, 111);
s.arp.ioctl(STCP_SIOCDARPENT, &req);
```


