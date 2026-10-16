#include "bedrock/keyboard.hpp"
#include <cassert>
#include <iostream>

struct EventLog {int downs = 0; int ups = 0;};
static void onKey(int key,bool down,bool repeated,void* ctx) {
    assert(key==0x41);
    assert(!repeated);
    auto* log=static_cast<EventLog*>(ctx);
    if(down)++log->downs;
    else ++log->ups;
}
int main() {
    bedrock::KeyboardObserver observer;
    EventLog log;
    observer.setKeyHandler(onKey,&log);
    assert(!observer.isDown('A'));
    observer.onWindowMessage(bedrock::KeyboardObserver::KeyDown,'A');
    assert(observer.isDown('A'));
    observer.onWindowMessage(bedrock::KeyboardObserver::KeyDown,'A',1ll<<30);
    assert(log.downs==1); // key repeat isn't new press.
    observer.onWindowMessage(bedrock::KeyboardObserver::KeyUp,'A');
    assert(!observer.isDown('A'));
    assert(log.ups==1);
    observer.onWindowMessage(bedrock::KeyboardObserver::KeyDown,'A');
    observer.onWindowMessage(bedrock::KeyboardObserver::KillFocus,0);
    assert(!observer.isDown('A'));
    assert(!observer.isDown(-1));
    assert(!observer.isDown(1000));
    std::cout<<"keyboard_test: PASS\n";
}
