"""Ensure a failed reply aborts only its own IPv4 TCP queue under the core lock."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
struct Ip{uint32_t addr;bool v4=true;};
struct tcp_pcb{tcp_pcb* next=nullptr;Ip local_ip{},remote_ip{};uint16_t local_port=80,remote_port=5000,snd_buf=5744,mss=1436,snd_queuelen=0;};
tcp_pcb* tcp_active_pcbs=nullptr;tcp_pcb* aborted=nullptr;
int locked=0,names=0;bool fails=false;sockaddr_in local{},peer{};
int fakeLocal(int fd,sockaddr* address,socklen_t*){assert(!locked && fd==7);++names;if(fails)return -1;memcpy(address,&local,sizeof(local));return 0;}
int fakePeer(int fd,sockaddr* address,socklen_t*){assert(!locked && fd==7);++names;memcpy(address,&peer,sizeof(peer));return 0;}
void tcp_abort(tcp_pcb* pcb){assert(locked==1 && !aborted);aborted=pcb;}
#define getsockname fakeLocal
#define getpeername fakePeer
#define LOCK_TCPIP_CORE() do{assert(!locked);++locked;}while(0)
#define UNLOCK_TCPIP_CORE() do{assert(locked==1);--locked;}while(0)
#define IP_IS_V4(p) ((p)->v4)
#define ip_2_ip4(p) (p)
#define ip4_addr_get_u32(p) ((p)->addr)
#define TCP_SND_BUF 5744
#include "http_transport_memory.cpp"
int main(){
 local.sin_family=peer.sin_family=AF_INET;local.sin_port=htons(80);peer.sin_port=htons(5000);
 local.sin_addr.s_addr=inet_addr("192.0.2.1");peer.sin_addr.s_addr=inet_addr("192.0.2.2");
 tcp_pcb wrongIp{},wrongPort{},ipv6{},target{};
 for(auto* p:{&wrongIp,&wrongPort,&ipv6,&target}){p->local_ip.addr=local.sin_addr.s_addr;p->remote_ip.addr=peer.sin_addr.s_addr;}
 wrongIp.remote_ip.addr=inet_addr("192.0.2.3");wrongPort.remote_port=6000;ipv6.remote_ip.v4=false;
 wrongIp.next=&wrongPort;wrongPort.next=&ipv6;ipv6.next=&target;tcp_active_pcbs=&wrongIp;
 configureHttpTransport(7);assert(target.mss==536 && !locked);
 assert(wrongIp.mss==1436 && wrongPort.mss==1436 && ipv6.mss==1436);
 for(auto peerMss:{256,536,1436}){
  target.mss=peerMss;configureHttpTransport(7);assert(target.mss==(peerMss<536?peerMss:536));
  assert(target.snd_buf==5744 && target.snd_queuelen==0);
 }
 target.mss=1436;target.snd_buf=5000;configureHttpTransport(7);assert(target.mss==1436);
 target.snd_buf=5744;target.snd_queuelen=1;configureHttpTransport(7);assert(target.mss==1436);
 target.snd_queuelen=0;configureHttpTransport(-1);assert(target.mss==1436);
 fails=true;configureHttpTransport(7);assert(target.mss==1436);fails=false;
 assert(httpSendAllowance(7)==3072 && !locked);
 for(auto queued:{0,1400,3072,5744}){target.snd_buf=5744-queued;assert(httpSendAllowance(7)==(queued<3072?3072-queued:0));assert(target.snd_buf==5744-queued);}
 names=0;
 abortHttpTransport(-1);assert(!names && !aborted && !locked);
 fails=true;abortHttpTransport(7);assert(names==1 && !aborted && !locked);
 fails=false;names=0;abortHttpTransport(7);assert(names==2 && aborted==&target && !locked);
 aborted=nullptr;peer.sin_port=htons(5001);assert(httpSendAllowance(7)==0);abortHttpTransport(7);assert(!aborted && !locked);
 peer.sin_family=AF_INET6;abortHttpTransport(7);assert(!aborted && !locked);
}
'''
@unittest.skipUnless(shutil.which('c++'),'host C++ compiler required')
class HttpTransportReleaseTests(unittest.TestCase):
 def test_failed_transmission_releases_only_the_matching_queue(self):
  with tempfile.TemporaryDirectory(prefix='ldg-http-release-') as directory:
   root=Path(directory);(root/'lwip/priv').mkdir(parents=True)
   for name in ('sockets.h','tcpip.h','priv/tcp_priv.h'):(root/'lwip'/name).write_text('')
   shutil.copyfile(ROOT/'src/http_transport_memory.cpp',root/'http_transport_memory.cpp')
   shutil.copyfile(ROOT/'include/http_transport_memory.h',root/'http_transport_memory.h')
   (root/'check.cpp').write_text('#include <initializer_list>\n'+HARNESS)
   subprocess.run(['c++','-std=c++11','-DARDUINO_ARCH_ESP32','-DBOARD_ESP32_3248S035C','-I',str(root),str(root/'check.cpp'),'-o',str(root/'check')],check=True,capture_output=True)
   result=subprocess.run([str(root/'check')],capture_output=True)
   self.assertEqual(result.returncode,0,result.stderr.decode())

 def test_other_profiles_keep_sdk_segment_size_and_send_allowance(self):
  with tempfile.TemporaryDirectory(prefix='ldg-http-sdk-segments-') as directory:
   root=Path(directory);(root/'lwip/priv').mkdir(parents=True)
   for name in ('sockets.h','tcpip.h','priv/tcp_priv.h'):(root/'lwip'/name).write_text('')
   shutil.copyfile(ROOT/'src/http_transport_memory.cpp',root/'http_transport_memory.cpp')
   shutil.copyfile(ROOT/'include/http_transport_memory.h',root/'http_transport_memory.h')
   main=r'''int main(){
    tcp_pcb pcb;tcp_active_pcbs=&pcb;
    for(auto fd:{-1,7}){configureHttpTransport(fd);assert(httpSendAllowance(fd)==size_t(-1));}
    assert(pcb.mss==1436 && pcb.snd_buf==5744 && !pcb.snd_queuelen && !names && !locked);
   }'''
   (root/'check.cpp').write_text('#include <initializer_list>\n'+HARNESS.split('int main(){',1)[0]+main)
   subprocess.run(['c++','-std=c++11','-DARDUINO_ARCH_ESP32','-I',str(root),str(root/'check.cpp'),'-o',str(root/'check')],check=True,capture_output=True)
   result=subprocess.run([str(root/'check')],capture_output=True)
   self.assertEqual(result.returncode,0,result.stderr.decode())
