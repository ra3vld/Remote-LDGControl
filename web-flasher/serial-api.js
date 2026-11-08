// Remote-LDGControl physical USB provisioning, version 1. GPL-3.0.
export const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
export class SerialApi {
 constructor(port,{timeout=2200,retries=3}={}){Object.assign(this,{port,timeout,retries,cid:0,lines:[],waiters:[],buffer:'',closed:false});this.queue=Promise.resolve();this.client=Array.from(crypto.getRandomValues(new Uint8Array(16)),n=>n.toString(16).padStart(2,'0')).join('');}
 async open(){
  await this.port.open({baudRate:115200,bufferSize:8192});
  try{await this.port.setSignals({dataTerminalReady:this.port.getInfo?.().usbVendorId===0x303a,requestToSend:false})}catch{}
  this.reader=this.port.readable.getReader();this.reading=this.readLoop();
  const deadline=Date.now()+30000;
  do{try{const hello=await this.exchange('hello',{client:this.client});if(hello.family!=='tuner'||!/^[a-f0-9]{12}$/.test(hello.device_id)||hello.device_id==='000000000000'||!hello.capabilities?.usb_provisioning)throw Error('Выберите совместимую прошивку LDG и её USB-порт.');this.boot=hello.boot;this.sid=hello.sid;this.identity=hello;this.keepalive=setInterval(()=>{if(this.pinging||this.closed)return;this.pinging=true;this.settings().catch(()=>{}).finally(()=>this.pinging=false)},2000);return this}catch(error){if(this.closed||Date.now()>=deadline)throw error;await pause(400)}}while(true);
 }
 async readLoop(){const decoder=new TextDecoder();try{while(!this.closed){const {value,done}=await this.reader.read();if(done)break;this.buffer+=decoder.decode(value,{stream:true});if(this.buffer.length>8192)throw Error('Слишком длинный ответ USB.');const lines=this.buffer.split(/[\r\n]+/);this.buffer=lines.pop();for(const line of lines){let packet;try{packet=JSON.parse(line)}catch{continue}if(packet?.ldg!==1)continue;if(this.waiters.length)this.waiters.shift()(packet);else{this.lines.push(packet);if(this.lines.length>16)this.lines.shift()}}}}catch(error){this.readError=error}finally{this.closed=true;this.reader.releaseLock();while(this.waiters.length)this.waiters.shift()(null)}}
 next(ms){if(this.lines.length)return Promise.resolve(this.lines.shift());if(this.closed)return Promise.reject(Error('USB отключён. Подключитесь и проверьте сохранённые настройки.'));return new Promise((resolve,reject)=>{const ready=packet=>{clearTimeout(timer);packet?resolve(packet):reject(Error('USB отключён.'))};const timer=setTimeout(()=>{const i=this.waiters.indexOf(ready);if(i>=0)this.waiters.splice(i,1);reject(Error('Нет ответа USB. Закройте монитор порта и повторите.'))},Math.max(1,ms));this.waiters.push(ready)})}
 exchange(op,fields={}){const result=this.queue.then(()=>this.perform(op,fields));this.queue=result.catch(()=>{});return result}
 async perform(op,fields={}){
  const cid=++this.cid,request=JSON.stringify({ldg:1,cid,op,...(op==='hello'?{}:{boot:this.boot,sid:this.sid}),...fields})+'\n';
  if(new TextEncoder().encode(request).length>1537)throw Error('Слишком длинные параметры USB.');
  for(let attempt=0;attempt<this.retries;attempt++){
   if(this.closed)throw Error('USB отключён. Переподключитесь для чтения настроек.');
   const writer=this.port.writable.getWriter();try{await writer.write(new TextEncoder().encode(request))}finally{writer.releaseLock()}
   const deadline=Date.now()+this.timeout;
   while(Date.now()<deadline){let reply;try{reply=await this.next(deadline-Date.now())}catch(error){if(this.closed)throw error;break}if(reply.cid!==cid)continue;
    if(op!=='hello'&&(reply.boot!==this.boot||reply.sid!==this.sid))throw Error('Устройство перезапустилось или изменился USB-сеанс. Подключитесь заново и проверьте настройки.');
    if(!Number.isInteger(reply.status)||reply.status<200||reply.status>=300)throw Error(reply.error||'USB-запрос отклонён.');return reply;
   }
  }throw Error('Сохранение не подтверждено. Повторите подключение и чтение настроек в этой вкладке.');
 }
 async settings(){return this.exchange('settings')}
 async apply(settings){return this.exchange('apply',{settings})}
 async close(){clearInterval(this.keepalive);this.closed=true;try{await this.reader?.cancel()}catch{}await this.reading;try{await this.port.close()}catch{}}
}
