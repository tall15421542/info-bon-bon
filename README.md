# info-bon-bon
It is a server program providing anonymous chat service. Most speciall of all, it allows user define their own filter!
The program would follow the filter function to match users, and create connection between users in miss match!
## Usage
### Start up server first
```
cd src
make
./inf-bonbon-server [port]
```
It will create an inf-bonbon-server in 0.0.0.0:[port]

## Testing(Provided by the TA of system programming class)
### STEP 1 - Configure the client infomation
```
cd test
vim info.json
```
For example·  
```
{
	"cmd": "matched",
	"name": "winner",                 // 名字長度不超過 32 字元
	"age": 23,                        // 年齡爲 0 ~ 99（請注意該數字兩側並無雙引號，它的型別是數字而非字串）
	"gender": "female",                 // 性別僅能爲 "female" 或 "male"
	// 自介爲不超過 1024 字元的英文
	"introduction": "so sorry...",
	// 篩選函式爲不超過 4096 字元的一個 C 語言函式，詳細說明請繼續往下看
	"filter_function": "int filter_function(struct User user) { return 1; }"
}
```
This function would receive one parameter, struct user, which is defined as follow  
```
struct User {
	char name[33],
	unsigned int age,
	char gender[7],
	char introduction[1025]
};
```
If the other client does not pass the filter, return 0;   
Otherwise, return an non-zero number.   
### STEP 2 - npm install
```
cd test
npm install
```
### STEP 3 - Connect to server
```
node client.js [ip] [port]
```
If the client succesfully connect to server, it would print
```
已連上伺服器。
請輸入下一步命令
[/t （嘗試匹配）]  [/c （結束網路連線）]
```
Then we can play with info-bon-bon chat system!
## More detailed spec
```
https://systemprogrammingatntu.github.io/MP4
```
