#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <assert.h>
#include <netinet/in.h>
#include "cJSON.h"

#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/signalfd.h>

#define MAX_FD 1025
#define MAX_JSON 6000
#define WorkingMatchNum 8

/* ICP protocol */

#define TRYMATCH 0x01
#define OVER 0x02
#define QUIT 0x14
#define SEND 0x15

/* User State */
#define OFFLINE 0x00
#define ONLINE  0x13
#define MATCHING 0x03
#define MATCHED 0x04

/* control to server */

#define FINDMATCH 0x05

/* Process State */
#define IDLE 0x00
#define ING 0x06
#define WAITING 0x07

/* Working Process protocol */
#define DOMATCH 0x08
#define ADDLIST 0x09
#define REMOVE  0x0A
#define MATCH 0x0B
#define ACCEPT 0x0C 
#define REJECT 0x0D
#define WAIT 0x0E

/* Working To Control */
#define FINISH 0x0F
#define HAVE 0x10
#define NOHAVE 0x11
#define NOMATCH 0x12

#define REG_S_FLAG (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

struct User {
	char name[33];
	unsigned int age;
	char gender[7];
	char introduction[1025];
};

struct User_Info{
    int client_fd;
    int matchFd;
    int state;
    struct User user;
};

/* implement list */
struct List_Node{
    struct User_Info User_Info;
    struct List_Node* next;
    struct List_Node* fore;
};

struct List{
    int num;
    struct List_Node* head;
    struct List_Node* tail;
};

void BroadcastOver(int OutToChild){
    int command = OVER;
    write(OutToChild, &command, sizeof(int));
}

void AddList(struct List* List, struct User_Info User_Info){
    struct List_Node* NewNode = (struct List_Node*)malloc(sizeof(struct List_Node));
    NewNode->User_Info = User_Info;
    NewNode->next = NULL;
    if(List->num == 0){
        NewNode->fore = NULL;
        List->head = NewNode;
        List->tail = NewNode;
    }
    else{
        NewNode->fore = List->tail;
        List->tail->next = NewNode;
        List->tail = NewNode;
    }
    List->num++;
}

int DeleteList(struct List* List){
    if(List->num == 0)
        return 0;
    struct List_Node* temp = List->head;
    List->head = List->head->next;
    if(List->head)
        List->head->fore = NULL;
    free(temp);
    List->num--;
    return 1;
}

int DeleteListFdSensitive(struct List* List, int fd){
    if(List->num == 0)
        return 0;
    struct List_Node* ptr = List->head;
    int find = 0;
    while(ptr){
        if(ptr->User_Info.client_fd == fd){
            find = 1;
            if(ptr == List->head){
                List->head = List->head->next;
            }
            else if(ptr == List->tail){
                List->tail = List->tail->fore;
            }
            if(ptr->fore)
                ptr->fore->next = ptr->next;
            if(ptr->next)
                ptr->next->fore = ptr->fore;
            free(ptr);
            List->num --;
            break;
        }
        ptr = ptr->next;
    }
    return find;
}

void GetString(cJSON* root, char* key, char* des){
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if(cJSON_IsString(item))
        strcpy(des, item->valuestring);
}

void GetValue(cJSON* root, char *key, unsigned int* des){
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if(cJSON_IsNumber(item))
        *des = (unsigned int)item->valuedouble;
}

void ParseTryMatch(cJSON* root, struct User* user){
    GetString(root, "name", user->name);
    GetString(root, "gender", user->gender);
    GetString(root, "introduction", user->introduction);
    GetValue(root, "age", &(user->age));
}

void NumToStr(int num, char* str){
    int index = 0;
    char temp[20];
    while(num!=0){
        temp[index] = (num % 10) + '0';
        num /= 10;
        index++;
    }
    for(int i = 0 ; i < index ; i++){
        str[i] = temp[index - i - 1];
    }
    str[index] = '\0';
    //printf("Library Check: %s\n", str);
}

void CreateMatchFunction(cJSON* root, int client_fd, char filterFun[][MAX_JSON]){
    char* STRUCT_USER = "struct User {char name[33];unsigned int age;char gender[7];char introduction[1025]; };";
    char Fun[5000];
    GetString(root, "filter_function", Fun);
    strcpy(filterFun[client_fd], Fun);
    char Library_Name[100];
    char Implement_Name[100];
    NumToStr(client_fd, Library_Name);
    NumToStr(client_fd, Implement_Name);
    strcat(Implement_Name, ".c");
    strcat(Library_Name, ".so");
    int fd = open(Implement_Name, O_WRONLY | O_TRUNC | O_CREAT, REG_S_FLAG);
    write(fd, STRUCT_USER, strlen(STRUCT_USER));
    int len = write(fd, Fun, strlen(Fun) );
    if(len != (strlen(Fun) ))
        //fprintf(stderr,"write error\n");
    close(fd);

    /* compile */
    char CompileCmd[100];
    sprintf(CompileCmd, "gcc -fPIC -O2 -std=c11 %s -shared -o %s", Implement_Name, Library_Name);
    //printf("%s\n", CompileCmd);
    system(CompileCmd);
}

void CompleteMessage(cJSON* root, char* MessageForClient){
    char message[1025];
    GetString(root, "message", message);
    unsigned int checkValue;
    GetValue(root, "sequence", &checkValue);
    sprintf(MessageForClient,"{" "\"cmd\":\"receive_message\"," "\"message\":\"%s\"," "\"sequence\":%d"
            "}\n", message, checkValue);
}

int Parse_JSON(char *text, struct User* user, int client_fd, char filterFun[][MAX_JSON], char* MessageForClient){
    cJSON* root = NULL;
    root = cJSON_Parse(text);
    cJSON* cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    int ret = 0;
    if(cJSON_IsString(cmd)){
        if(strcmp(cmd->valuestring, "try_match") == 0){
            ParseTryMatch(root, user);
            CreateMatchFunction(root, client_fd, filterFun);
            ret = TRYMATCH;
        }
        else if(strcmp(cmd->valuestring, "quit") == 0){
            ret = QUIT;
        }
        else if(strcmp(cmd->valuestring, "send_message")==0){
            CompleteMessage(root, MessageForClient);
            ret = SEND;
        }
    }
    cJSON_Delete(root);
    return ret;
}

void PrintUser(struct User user){
    //fprintf(stderr,"name: %s\n", user.name);
    //fprintf(stderr,"age: %u\n", user.age);
    //fprintf(stderr,"gender: %s\n", user.gender);
    //fprintf(stderr,"intro: %s\n", user.introduction);
}

int StrToNum(char* str){
    int len = strlen(str);
    int ret = 0;
    for(int i = 0 ; i < len ; i++){
        ret *= 10;
        ret += str[i] - '0';
    }
    return ret;
}

struct OKList_Node{
    int User_Id;
    struct List List;
    struct OKList_Node* next;
    struct OKList_Node* fore;
};

struct OKList{
    int num;
    struct OKList_Node* head;
    struct OKList_Node* tail;
};

void AddOKList (struct OKList* OKList, int User_Id, struct List List){
    struct OKList_Node* New_Node = (struct OKList_Node*)malloc(sizeof(struct OKList_Node));
    New_Node -> User_Id = User_Id;
    New_Node -> List = List;
    New_Node -> next = NULL;
    if(OKList->num == 0){
        New_Node -> fore = NULL;
        OKList->head = New_Node;
        OKList->tail = New_Node;
    }
    else{
        New_Node -> fore = OKList->tail;
        OKList->tail->next = New_Node;
        OKList->tail = New_Node;
    }
    OKList->num ++;
}

void ClearOKList(struct OKList_Node* Node){
    struct List_Node* ptr = Node->List.head;
    while(ptr){
        struct List_Node* temp = ptr->next;
        free(ptr);
        ptr = temp;
    }
    free(Node);
}

int DeleteOKList(struct OKList* OKList){
    if(OKList->num == 0)
        return 0;
    struct OKList_Node* temp = OKList->head;
    OKList->head = OKList->head->next;
    if(OKList->head)
        OKList->head->fore = NULL;
    ClearOKList(temp);
    OKList->num--;
    return 1;
}

void CreatPipe(char* input_pipe, char* output_pipe, int* input_fd, int* output_fd ){
    
    int ret;
    /* name pipe */
    remove(input_pipe);
    ret = mkfifo(input_pipe, 0644);
    //assert (!ret);

    remove(output_pipe);
    ret = mkfifo(output_pipe, 0644);
    //assert (!ret);

    /* open pipes */
    *input_fd = open(input_pipe, O_RDWR);
    assert (*input_fd >= 0);

    *output_fd = open(output_pipe, O_RDWR);
    assert (*output_fd >= 0); 
}

void CreatOneWayPipe_In(char* input_pipe){
    int ret;
    ret = mkfifo(input_pipe, 0644);
}

int CheckMatch(int FdOpenLibrary, struct User User, char* FunCommunicate_In){
    //printf("Deal with Match....\n");
    char Library_Name[30];
    NumToStr(FdOpenLibrary, Library_Name);
    strcat(Library_Name,".so");
    char RelatedPath[30];
    sprintf(RelatedPath, "./%s", Library_Name);
    //printf("Library: %s\n", RelatedPath);
    void* handle = dlopen(RelatedPath, RTLD_LAZY);
    if( handle == NULL){
        fprintf(stderr, "Fail to open\n");
        dlclose(handle);
        return 0;
    }

    // 載入 multiply 函數
    dlerror();
    int (*filter_fn)(struct User) = (int (*)(struct User)) dlsym(handle, "filter_function");


    // 若找不到函數進行錯誤處理
    const char *dlsym_error = dlerror();
    if (dlsym_error)
    {
        fprintf(stderr, "Cannot load symbol 'multiple': %s\n", dlsym_error);
        dlclose(handle);
        return 0;
    }
    int result = 0;
    // 使用動態載入的函數
    pid_t pid;
    int InFromFun;
    CreatOneWayPipe_In(FunCommunicate_In);
    InFromFun = open(FunCommunicate_In, O_RDWR);
    assert(InFromFun >= 0);

    
    if( (pid = fork()) == 0){
        //printf("Calculate...\n");
        close(InFromFun);
        InFromFun = open(FunCommunicate_In, O_RDWR);
        result = filter_fn(User);
        assert(InFromFun >= 0);
        write(InFromFun, &result, sizeof(int));
        dlclose(handle);
        close(InFromFun);
        //printf("Finish fork() mission\n");
        exit(0);
    }
    else{
        int status;
        waitpid(pid, &status, 0);
        if(WIFEXITED(status)){
            read(InFromFun, &result, sizeof(int));
           // printf("result: %d\n", result);
        }
        close(InFromFun);
    }
    // 最後記得關閉 handle
    dlclose(handle);
    
    //printf("complete Match\n");
    return result;

}

int MatchUserWithinList(struct List* List, struct List* Confirm_LIST, struct User_Info User_Info, char* FunCommunicate_In){
    struct List_Node* ptr = List->head;
    //fprintf(stderr, "In %s\n", FunCommunicate_In);
    int HaveMatch = 0;
    while(ptr){
        //fprintf(stderr, "Match with %d...\n", ptr->User_Info.client_fd);
        if(User_Info.client_fd != ptr->User_Info.client_fd &&
           CheckMatch(ptr->User_Info.client_fd, User_Info.user, FunCommunicate_In) && 
           CheckMatch(User_Info.client_fd, ptr->User_Info.user, FunCommunicate_In)){
            AddList(Confirm_LIST, ptr->User_Info);
            //fprintf(stderr, "Match: %s(%d)-%s(%d)\n", User_Info.user.name, User_Info.client_fd, ptr->User_Info.user.name, ptr->User_Info.client_fd);
            HaveMatch = 1;
        }
        ptr = ptr->next;
        //fprintf(stderr, "safe\n");
    }
    //fprintf(stderr, "%d complete matching\n", User_Info.client_fd);
    return HaveMatch;
}

void PrintList(struct List List){
    struct List_Node* ptr = List.head;
    while(ptr){
        //fprintf(stderr, "%d-", ptr->User_Info.client_fd);
        ptr = ptr->next;
    }
    //fprintf(stderr, "\n");
}

void ReceiveAddList(int InFromParent, struct List* List){
    struct User_Info Receive;
    read(InFromParent, &Receive, sizeof(struct User_Info));
    AddList(List, Receive);
    /*int ReceiveNum;
    read(InFromParent, &ReceiveNum, sizeof(int));
    printf("Num To Add: %d\n", ReceiveNum);
    for(int i = 0 ; i < ReceiveNum ; i++){
        struct User_Info Receive;
        read(InFromParent, &Receive, sizeof(struct User_Info));
        //PrintUser(Receive.user);
        AddList(List, Receive);
    }*/
}

void TellProcessState(int OutToParent, int OKListNum){
    int state;
    if(OKListNum == 0){
        state = IDLE;
        //fprintf(stderr, "Child is IDLE\n");
        write(OutToParent, &state, sizeof(int));
        return;
    }
    state = WAIT;
    //fprintf(stderr, "Child is wait to clear\n");
    write(OutToParent, &state, sizeof(int));
    return;
}

void TellParentFinishMatch(int OutToParent, int InFromParent, int IfMatch, struct OKList* OK_List, struct List* List){
    int message;
    if(IfMatch == 0){
        message = NOMATCH;
        write(OutToParent, &message, sizeof(int));
        TellProcessState(OutToParent, OK_List->num);
        return;
    }
    message = FINISH;
    write(OutToParent, &message, sizeof(int));
    int state; /* Is the first client? */
    read(InFromParent, &state, sizeof(int));
    if(state == WAIT){
        //fprintf(stderr, "Wait to update...\n");
        return;
    }
    write(OutToParent, &OK_List->head->User_Id, sizeof(int));
    read(InFromParent, &state, sizeof(int));
    if(state == REJECT){
        DeleteOKList(OK_List);
        TellProcessState(OutToParent, OK_List->num);
        return;
    }
    struct List TransmitList = OK_List->head->List;
    struct List_Node* ptr = TransmitList.head;
    while(ptr){
        message = HAVE;
        write(OutToParent, &message, sizeof(int));
        write(OutToParent, &ptr->User_Info.client_fd, sizeof(int));
        int acceptState;
        read(InFromParent, &acceptState, sizeof(int)); 
        if(acceptState == ACCEPT){
            //fprintf(stderr, "Accept %d-%d\n", OK_List->head->User_Id, ptr->User_Info.client_fd);
            DeleteListFdSensitive(List, OK_List->head->User_Id);
            DeleteListFdSensitive(List, ptr->User_Info.client_fd);
            DeleteOKList(OK_List);
            //fprintf(stderr, "Success Delete Ok List\n");
            TellProcessState(OutToParent, OK_List->num);
            return;
        }
        DeleteListFdSensitive(List, ptr->User_Info.client_fd);
        ptr = ptr->next;
    }
    message = NOHAVE;
    write(OutToParent, &message, sizeof(int));
    DeleteOKList(OK_List);
    TellProcessState(OutToParent, OK_List->num);
}

void CompleteMatchMessage(char* responseToA, struct User_Info User_Info, char* filterFun){
    struct User User = User_Info.user;
    sprintf(responseToA,
            "{" "\"cmd\":\"matched\"," "\"name\":\"%s\","
            "\"age\":%d," "\"gender\":\"%s\","
           "\"introduction\":\"%s\"," "\"filter_function\":\"%s\"" "}\n",
            User.name, User.age, User.gender, User.introduction, filterFun);
}

void ListenMatchInServer(int InFromChild, struct User_Info User_Info[], char filterFun[][MAX_JSON]){
    int match_a, match_b;
    read(InFromChild, &match_a, sizeof(int));
    read(InFromChild, &match_b, sizeof(int));
    User_Info[match_a].matchFd = match_b;
    User_Info[match_a].state = MATCHED;
    User_Info[match_b].matchFd = match_a;
    User_Info[match_b].state = MATCHED;
    char responseToA[MAX_JSON];
    CompleteMatchMessage(responseToA, User_Info[match_b], filterFun[match_b]);
    char responseToB[MAX_JSON];
    CompleteMatchMessage(responseToB, User_Info[match_a], filterFun[match_a]);
    send(match_a, responseToA, strlen(responseToA), 0);
    send(match_b, responseToB, strlen(responseToB), 0);
    //fprintf(stderr, "successfully match!\n");
}

void TellServerMatch(int OutToParent, int a, int b){
    int message = FINDMATCH;
    write(OutToParent, &message, sizeof(int));
    write(OutToParent, &a, sizeof(int));
    write(OutToParent, &b, sizeof(int));
}

void ListenChildState(int InFromChild, int *ProcessState){
    int state;
    read(InFromChild, &state, sizeof(int));
    //printf("Parent receive %d\n", state);
    *ProcessState = state;
}

int ListenToChild(int OutToParent, int OutToChild, int InFromChild, int Table[], int *ProcessState, int* match_a, int* match_b){
    int MatchUser;
    int message;
    read(InFromChild, &MatchUser, sizeof(int));
    if(!Table[MatchUser]){
        message = REJECT;
        write(OutToChild, &message, sizeof(int));
        ListenChildState(InFromChild, ProcessState);
        return 0;
    }
    else{
        message = ACCEPT;
        write(OutToChild, &message, sizeof(int));
    }
    while(1){
        read(InFromChild, &message, sizeof(int));
        if(message == HAVE){
            int UserInList;
            read(InFromChild, &UserInList, sizeof(int));
            if(!Table[UserInList]){
                //fprintf(stderr, "Reject %d because matched\n", UserInList);
                message = REJECT;
                write(OutToChild, &message, sizeof(int));
            }
            else{
                message = ACCEPT;
                write(OutToChild, &message, sizeof(int));
                ListenChildState(InFromChild, ProcessState);
                Table[MatchUser] = 0;
                Table[UserInList] = 0;
                //fprintf(stderr, "%d and %d is matched and not avilable\n", MatchUser, UserInList);
                *match_a = MatchUser;
                *match_b = UserInList;
                return 1;
            }
        }
        else{
            ListenChildState(InFromChild, ProcessState);
            return 0;
        }
    }
}

void WorkingMatch(int InFromParent, int OutToParent, char* FunCommunicate_In){
    //printf("Working Matching!\n");
    fd_set ReadSet;
    fd_set WorkingReadSet;
    FD_ZERO(&ReadSet);

    FD_SET(InFromParent, &ReadSet);
    int Maxfd = InFromParent + 1;

    struct List List;
    memset(&List, 0, sizeof(struct List));
    struct OKList OKList;
    memset(&OKList, 0, sizeof(struct OKList));
    while(1){
        if(OKList.num > 0){
            TellParentFinishMatch(OutToParent, InFromParent, 1, &OKList, &List);
        }
        memcpy(&WorkingReadSet, &ReadSet, sizeof(fd_set));
        int ret = select(Maxfd, &WorkingReadSet, NULL, NULL, NULL);
        if(ret < 0){
            //fprintf(stderr, "GrandSon\n");
            perror("select() went wrong");
            exit(errno);
        }
        if(ret == 0)
            continue;
        else if(FD_ISSET(InFromParent, &WorkingReadSet)){
            int command;
            int ret = read(InFromParent, &command, sizeof(int));
            if(ret == 0){
                continue;
            }
            //fprintf(stderr, "Print List before doing every thing int %s\n", FunCommunicate_In);
            PrintList(List);
            //printf("In the Grandson\n");
            switch(command){
                case DOMATCH:
                {
                    //fprintf(stderr, "Do Match\n");
                    PrintList(List);
                    struct User_Info User_Info;
                    read(InFromParent, &User_Info, sizeof(struct User_Info));
                    
                    //PrintUser(User_Info.user);
                    struct List ConfirmList;
                    memset(&ConfirmList, 0, sizeof(struct List));
                    int IfMatch = MatchUserWithinList(&List, &ConfirmList, User_Info, FunCommunicate_In);
                    if(IfMatch)
                    { 
                        AddOKList(&OKList, User_Info.client_fd, ConfirmList);
                        //fprintf(stderr, "confirm list:\n");
                        //PrintList(ConfirmList);
      //                  printf("Waiting %d\n", OKList.num);
                    }
                    else{
                        //fprintf(stderr, "No Match\n");
                        AddList(&List, User_Info);
                    }
                    TellParentFinishMatch(OutToParent, InFromParent, IfMatch, &OKList, &List);
                    //fprintf(stderr, "Print List after do match in %s\n", FunCommunicate_In);
                    PrintList(List);
                    break;
                }
                case ADDLIST:
                {
                    //fprintf(stderr, "Add list\n"); 
                    ReceiveAddList(InFromParent, &List);
                    //printf("Print List after add list int %s\n", FunCommunicate_In);
                    //PrintList(List);
                    break;
                }
                case REMOVE:
                {
                    int RemoveFd;
                    read(InFromParent, &RemoveFd, sizeof(int));
                    DeleteListFdSensitive(&List, RemoveFd);
                    //printf("Remove %d\n", RemoveFd);
                    //printf("Print List after remove in %s\n", FunCommunicate_In);
                    //PrintList(List);
                    break;                       
                }

                case OVER:
                {
                    //printf("Grandson Over\n");
                    exit(0);
                    break;
                }
                default:
                    break;
                    //printf("Unknown command %d\n", command);
            }
        }
    }
}

void CreateWorkingProcess(pid_t* Pid, char* WorkingMatchProcess_In, char* WorkingMatchProcess_Out, char* FunCommunicate_In){
    pid_t pid;
    if( (pid = fork()) == 0){
        int WorkingInFd, WorkingOutFd;
        CreatPipe(WorkingMatchProcess_In, WorkingMatchProcess_Out, &WorkingInFd, &WorkingOutFd);
        //fprintf(stderr, "Grandson\n");
        //printf("OutPut Fd: %d, InPutFd: %d\n", WorkingInFd, WorkingOutFd);
        WorkingMatch(WorkingInFd, WorkingOutFd, FunCommunicate_In);
        exit(0);
    }
    else{
        *Pid = pid;
        return;
    }
}

void TransmmitAddList(struct List* AddList, int OutToChild){
    int command = ADDLIST;
    int TransmmitNum = AddList->num;
    write(OutToChild, &command, sizeof(int));
    write(OutToChild, &TransmmitNum, sizeof(int));
    for(int i = 0 ; i < TransmmitNum ; i++){
        struct User_Info Transmmit = AddList->head->User_Info;
        write(OutToChild, &Transmmit, sizeof(struct User_Info));
        DeleteList(AddList);
    }
}

void BroadCastWorkingAdd(struct User_Info User_Info, struct List WorkingList[], struct List Add_List[], int ProcessState[], int* ProcessMatch, int OutToChild[]){
    //printf("BroadCast\n");
    for(int i = 0; i < WorkingMatchNum ; i++){
        //printf("Add %d to add_list for process %d\n", User_Info.client_fd, i);
        if(i == *ProcessMatch){
            AddList(&WorkingList[i], User_Info);
            AddList(&Add_List[i], User_Info);
            /*if(ProcessState[i] == IDLE || ProcessState[i] == WAITING){
                int command = DOMATCH;
                struct User_Info Transmmit = WorkingList[i].head->User_Info;
                write(OutToChild[i], &command, sizeof(int));
                write(OutToChild[i], &Transmmit, sizeof(struct User_Info));
                ProcessState[i] = ING;
                DeleteList(&WorkingList[i]);
            }
            else{
                printf("working\n");
            }*/
        }
        else{
            AddList(&Add_List[i], User_Info);
            /*if(ProcessState[i] == IDLE || ProcessState[i] == WAITING){
                TransmmitAddList(&Add_List[i], OutToChild[i]);
            }
            else{
                printf("working\n");
            }*/
        }
    }
    *ProcessMatch = (*ProcessMatch + 1) % WorkingMatchNum ;
}

void BroadstWorkMatch(struct List Add_List[], struct List WorkingList[], int ProcessState[], int OutToChild[]){
    for(int i = 0 ; i < WorkingMatchNum ; i++){
        if(WorkingList[i].num > 0){
            /* no time to care new job*/
            if(ProcessState[i] == ING)
                continue;
            /* add list before work */
            int WorkFd = WorkingList[i].head->User_Info.client_fd;
            while(1){
                if(Add_List[i].head->User_Info.client_fd != WorkFd){
                    //printf("Process %d add %d\n", i, Add_List[i].head->User_Info.client_fd);
                    int command = ADDLIST;
                    write(OutToChild[i], &command, sizeof(int));
                    struct User_Info Transmmit = Add_List[i].head->User_Info;
                    write(OutToChild[i], &Transmmit, sizeof(struct User_Info));
                    DeleteList(&Add_List[i]);
                }
                else{
                    int command = DOMATCH;
                    struct User_Info Transmmit = WorkingList[i].head->User_Info;
                    write(OutToChild[i], &command, sizeof(int));
                    //printf("before send work match user %d\n", WorkingList[i].head->User_Info.client_fd);
                    write(OutToChild[i], &Transmmit, sizeof(struct User_Info));
                    //printf("finish send work\n");
                    ProcessState[i] = ING;
                    DeleteList(&WorkingList[i]);
                    DeleteList(&Add_List[i]);
                    break;
                }
            }
        }
    }
}
void UpdateList(struct List Close_List[], struct List Add_List[], struct List Working_List[], int ProcessState[], int OutToChild[]){
    for(int i = 0 ; i < WorkingMatchNum ; i++){
        if(ProcessState[i] == ING){
            //printf("Process %d is busy\n", i);
            continue;
        }
        while(Close_List[i].num > 0){
            DeleteListFdSensitive(&Working_List[i], Close_List[i].head->User_Info.client_fd);
            DeleteListFdSensitive(&Add_List[i], Close_List[i].head->User_Info.client_fd);
            int command = REMOVE;
            write(OutToChild[i], &command, sizeof(int));
            write(OutToChild[i], &Close_List[i].head->User_Info.client_fd, sizeof(int));
            DeleteList(&Close_List[i]);
        }
    }
}

int FindChildFd(int fd, int InFromChild[]){
    for(int index = 0 ; index < WorkingMatchNum ; index++){
        if(fd == InFromChild[index])
            return index;
    }
    //printf("Not Found ChildFd\n");
    return -1;
}
void AddCloseList(struct List Close_List[], int a){
    for(int i = 0 ; i < WorkingMatchNum ; i++){
        struct User_Info temp;
        temp.client_fd = a;
        AddList(&Close_List[i], temp);
    }
}

void HandleMatch(int InFromParent, int OutToParent, int* InFromChild, int *OutToChild ){
    //printf("Handle working\n");

    /* initialize select related */
    fd_set ReadSet;
    fd_set WorkingReadSet;
    FD_ZERO(&ReadSet);
    FD_SET(InFromParent, &ReadSet);
    //printf("select In From Parent %d\n", InFromParent);
    for(int i = 0 ; i < WorkingMatchNum; i++){
        FD_SET(InFromChild[i], &ReadSet);
      //  printf("select In From Child %d\n", InFromChild[i]);
    }
    
    /* initialize chile process state */
    
    int ProcessState[WorkingMatchNum] = {};
    struct List Add_List[WorkingMatchNum] = {};
    struct List Close_List[WorkingMatchNum] = {};
    struct List WorkingList[WorkingMatchNum] = {};
    int ProcessMatch = 0;
    int ProcessDealFirst = 0;
    int Table[MAX_FD] = { };
    int max_fd = 0;
    for(int i = 0 ; i < WorkingMatchNum ; i++){
        if(max_fd < InFromChild[i])
            max_fd = InFromChild[i];
    }
    if(max_fd < InFromParent)
        max_fd = InFromParent;
    max_fd++;

    while(1){
        /*printf("delete list\n");
        for(int i = 0 ; i < WorkingMatchNum ; i++)
            PrintList(Close_List[i]);*/
        UpdateList(Close_List, Add_List, WorkingList, ProcessState, OutToChild);
        BroadstWorkMatch(Add_List, WorkingList, ProcessState, OutToChild);
        memcpy(&WorkingReadSet, &ReadSet, sizeof(fd_set));
        int ret = select(max_fd, &WorkingReadSet, NULL, NULL, NULL);
        if(ret < 0){
            printf("child\n");
            perror("select() went wrong");
            exit(errno);
        }
        else if( ret == 0){
            continue;
        }
            
        for(int fd = 0 ; fd < max_fd ; fd+=1){
            if(!FD_ISSET(fd, &WorkingReadSet))
                continue;        
            if (fd == InFromParent){ /* from parent server */
                int command;
                int ret = read(InFromParent, &command, sizeof(int));
                if(ret == 0){
                    continue;
                }
        //        printf("command: %d\n", command);
                switch(command){
                    case TRYMATCH:{
                        struct User_Info DealWith;
                        read(InFromParent, &DealWith, sizeof(struct User_Info));
                        //printf("Try Match in the child process\n");
                        //PrintUser(DealWith.user);
                        Table[DealWith.client_fd] = 1;
                        //printf("Set %d Available\n", DealWith.client_fd);
                        BroadCastWorkingAdd(DealWith, WorkingList, Add_List, ProcessState, &ProcessMatch, OutToChild);
                        break;         
                    }
                    case QUIT:{
                        int close_fd;
                        read(InFromParent, &close_fd, sizeof(int));
                        Table[close_fd] = 0;
                        //printf("%d is quit and not avilable\n", close_fd);
                        AddCloseList(Close_List, close_fd);
                        break;
                    }
                    case OVER:
                    {
                        for(int i = 0 ; i < WorkingMatchNum ; i++)
                            BroadcastOver(OutToChild[i]);
                        //printf("Child Over\n");
                        sleep(1);
                        exit(0);
                    }
                 
                } 
            }
            /* From the working child */
             else { 
                //printf("Child Fd: %d\n", fd);
                int FdIndex =  FindChildFd(fd, InFromChild);
                int OutFd = OutToChild[FdIndex];
                int message;
                read(fd, &message, sizeof(int));
                if(FdIndex == -1){
                    //printf("unknown message: %d\n", message);
                    //printf("unknown Fd %d\n", fd);
                    continue;
                }
                if(fd == InFromChild[ProcessDealFirst]){
                    if(message == FINISH){
                        message = ACCEPT;
                        int MatchA, MatchB;
                        write(OutToChild[ProcessDealFirst], &message, sizeof(int));
                        if(ListenToChild(OutToParent, OutToChild[ProcessDealFirst], InFromChild[ProcessDealFirst], 
                                         Table, &ProcessState[ProcessDealFirst], &MatchA, &MatchB)){
                            TellServerMatch(OutToParent, MatchA, MatchB);
                            AddCloseList(Close_List, MatchA);
                            AddCloseList(Close_List, MatchB);
                        }
                    }
                    else if(message == NOMATCH){
                        //printf("No Match\n");
                        ListenChildState(fd, &ProcessState[FdIndex]);
                    }
                    ProcessDealFirst = (ProcessDealFirst + 1) % WorkingMatchNum;
                }
                else{
                    if(message == FINISH){
                        message = WAIT;
                        write(OutFd, &message, sizeof(int));
                        ProcessState[FdIndex] = WAIT;
                    }
                    else if(message == NOMATCH){
                        ListenChildState(fd, &ProcessState[FdIndex]);
                    }
                }
                //printf("First Process %d\n", ProcessDealFirst);
            }
            
        }
    }
}

void CreateHandleMatchProcess(pid_t* Pid, char* ControlMatchProcess_In, char* ControlMatchProcess_Out, int sockfd){
    pid_t pid;
    if( (pid = fork()) == 0){
        close(sockfd);
        char WorkingMatchProcess_In[WorkingMatchNum][30] = { "Grandson-1.in", "Grandson-2.in", "Grandson-3.in", "Grandson-4.in",
                                                             "Grandson-5.in", "Grandson-6.in", "Grandson-7.in", "Grandson-8.in" };
        char WorkingMatchProcess_Out[WorkingMatchNum][30] = { "Grandson-1.out", "Grandson-2.out", "Grandson-3.out", "Grandson-4.out",
                                                              "Grandson-5.out", "Grandson-6.out", "Grandson-7.out", "Grandson-8.out" };
        char FunCommunicate_In[WorkingMatchNum][30] = { "Fun-1.in", "Fun-2.in", "Fun-3.in", "Fun-4.in", 
                                                        "Fun-5.in", "Fun-6.in", "Fun-7.in", "Fun-8.in"};
        pid_t MatchPid[WorkingMatchNum] = {};
        int InFromChild[WorkingMatchNum];
        int OutToChild[WorkingMatchNum];
        for(int i = 0 ; i < WorkingMatchNum ; i++){
            CreateWorkingProcess(&MatchPid[i], WorkingMatchProcess_In[i], WorkingMatchProcess_Out[i], FunCommunicate_In[i]);
        }
        sleep(1);
        for(int i = 0 ; i < WorkingMatchNum ; i++){
            OutToChild[i] = open(WorkingMatchProcess_In[i], O_RDWR);
            InFromChild[i] = open(WorkingMatchProcess_Out[i], O_RDWR);
          //  printf("Process %d In Fd: %d\n", i, InFromChild[i]);
          //  printf("Process %d Out Fd: %d", i, OutToChild[i]);
        }
        int InFromParent, OutToParent;
        CreatPipe(ControlMatchProcess_In, ControlMatchProcess_Out, &InFromParent, &OutToParent);
        //fprintf(stderr, "Child\n");
        HandleMatch(InFromParent, OutToParent, InFromChild, OutToChild);
        exit(0);
    }
    else{
        *Pid = pid;
        return;
    }
}

void CompleteUserInfo(struct User_Info* User_Info, int client_fd, int matchFd, int state){
    User_Info->client_fd = client_fd;
    User_Info->matchFd = -1;
    User_Info->state = state;
}


void TellClientQuit(int match_fd){
    char* message = "{\"cmd\":\"other_side_quit\"}\n";
    send(match_fd, message, strlen(message), 0);
}

void PrintUserInfo(struct User_Info User_Info){
    //printf("client_fd %d\n", User_Info.client_fd);
    //printf("match_fd %d\n", User_Info.matchFd);
    //printf("state %d\n", User_Info.state );
    //PrintUser(User_Info.user);
}

int main(int argc, char** argv){

    // 宣告 socket 檔案描述子
    int port = StrToNum(argv[1]);
    //printf("port %d\n", port);
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(sockfd >= 0);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr)); // 清零初始化，不可省略
    server_addr.sin_family = PF_INET;              // 位置類型是網際網路位置
    server_addr.sin_addr.s_addr = INADDR_ANY;      // INADDR_ANY 是特殊的IP位置，表示接受所有外來的連線
    server_addr.sin_port = htons(port);           // 在 44444 號 TCP 埠口監聽新連線

    // 綁定位置
    int retval = bind(sockfd, (struct sockaddr*) &server_addr, sizeof(server_addr)); // 綁定 sockfd 的位置
    if(retval != 0){
        printf("socket fail\n");
        //BroadcastOver(OutToChild);
        close(sockfd);
        //sleep(1);
        exit(0);
    }
   // printf("successfully bindding\n");
    // listen

    retval = listen(sockfd, 5);
    assert(!retval);
   // printf("successfully listening\n");
    pid_t Pid;
    char* ControlMatchProcess_In = "Child.in";
    char* ControlMatchProcess_Out = "Child.out";   
    CreateHandleMatchProcess(&Pid, ControlMatchProcess_In, ControlMatchProcess_Out, sockfd);
    
    sleep(2);
    int OutToChild = open(ControlMatchProcess_In, O_RDWR);
    int InFromChild = open(ControlMatchProcess_Out, O_RDWR);
   // printf("InFromChild %d\n", InFromChild);

    /* set signalfd */
    sigset_t mask;
    int sfd;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    sfd = signalfd(-1, &mask, 0);

    // 宣告 select() 使用的資料結構
    fd_set readset;
    fd_set working_readset;
    FD_ZERO(&readset);

    // 將 socket 檔案描述子放進 readset
    FD_SET(sockfd, &readset);
    FD_SET(InFromChild, &readset);
    FD_SET(STDIN_FILENO, &readset);
    FD_SET(sfd, &readset);

    struct User_Info User_Info[MAX_FD] = {};
    char BUFFER[MAX_FD][MAX_JSON];
    static char filterFun[MAX_FD][MAX_JSON];

    while (1)
    {
        memcpy(&working_readset, &readset, sizeof(fd_set));
        int retval = select(MAX_FD, &working_readset, NULL, NULL, NULL);

        if (retval < 0) // 發生錯誤
        {
            printf("main()\n");
            BroadcastOver(OutToChild);
            perror("select() went wrong");
            exit(errno);
        }

        if (retval == 0) // 排除沒有事件的情形
            continue;

        for (int fd = 0; fd < MAX_FD; fd += 1) // 用迴圈列舉描述子
        {
            // 排除沒有事件的描述子
            if (!FD_ISSET(fd, &working_readset))
                continue;
            if(fd == sfd){
                struct signalfd_siginfo fdsi;
                ssize_t s;
                s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
                if(fdsi.ssi_status == SIGCHLD){
                    //printf("child exit\n");
                    exit(0);
                }
                continue;
            }

            if (fd == STDIN_FILENO){
     //           printf("Standard In\n");
                char command[20];
                scanf("%s", command);
                //printf("command %s\nend_command\n", command);
                if(strcmp(command, "over") == 0){
                    BroadcastOver(OutToChild);
                    close(sockfd);
                    FD_CLR(sockfd, &readset);
                    sleep(1);
                    exit(0);
                }
            }

            // 分成兩個情形：接受新連線用的 socket 和資料傳輸用的 socket
            if (fd == sockfd)
            {
                // sockfd 有事件，表示有新連線
                struct sockaddr_in client_addr;
                socklen_t addrlen = sizeof(client_addr);
                int client_fd = accept(fd, (struct sockaddr*) &client_addr, &addrlen);
                if (client_fd >= 0)
                    FD_SET(client_fd, &readset); // 加入新創的描述子，用於和客戶端連線
                //printf("New Connect: %d\n", client_fd);
                User_Info[client_fd].state = ONLINE;
            }
            else if(fd == InFromChild){
                int message;
                read(InFromChild, &message, sizeof(int));
                if(message == FINDMATCH)
                    ListenMatchInServer(InFromChild, User_Info, filterFun);
            }
            else
            {
                // 這裏的描述子來自 accept() 回傳值，用於和客戶端連線
                ssize_t sz;
                char buffer[MAX_JSON];
                sz = recv(fd, buffer, MAX_JSON, 0); // 接收資料

                if (sz == 0) // recv() 回傳值爲零表示客戶端已關閉連線
                {
                    // 關閉描述子並從 readset 移除
                    close(fd);
                    FD_CLR(fd, &readset);
                    if(User_Info[fd].state == MATCHED){
                        //printf("Matheed but quit\n");
                        TellClientQuit(User_Info[fd].matchFd);
                        User_Info[User_Info[fd].matchFd].state = ONLINE;
                    }
                    else if(User_Info[fd].state == MATCHING){
                        //printf("Matching but Quit\n");
                        int command = QUIT;
                        write(OutToChild, &command, sizeof(int));
                        write(OutToChild, &fd, sizeof(int));
                    }
                    memset(&User_Info[fd], 0, sizeof(struct User_Info));
                    memset(BUFFER[fd], 0, MAX_JSON);
                }
                else if (sz < 0) // 發生錯誤
                {
                    /* 進行錯誤處理
                       ...略...  */
                }
                else // sz > 0，表示有新資料讀入
                {
                    int CutPoint = 0;
                    for(int pos = 0 ; pos < sz ; pos++){
                        if(buffer[pos] == '\n'){
                            strncat(BUFFER[fd], &buffer[CutPoint], pos - CutPoint + 1);
                            //printf("Client To server\n%s\n", BUFFER[fd]);
                            char MessageForClient[MAX_JSON];
                            int command = Parse_JSON(BUFFER[fd], &(User_Info[fd].user), fd, filterFun, MessageForClient);
                            switch(command){
                                case TRYMATCH:{
                                    /* check State of User First */
                                    if(User_Info[fd].state !=  ONLINE){
                                        //printf("Not Online but try match\n");
                                        break;
                                    }
                                    /* send user to child process who control match */
                                    CompleteUserInfo(&User_Info[fd], fd, -1, MATCHING);
                                    //PrintUserInfo(User_Info[fd]);
                                    int cmd = TRYMATCH;
                                    write(OutToChild, &cmd, sizeof(int));
                                    write(OutToChild, &User_Info[fd], sizeof(struct User_Info));
                                    //printf("TRY MATCH in server\n");
                                    /* tell client message */
                                    char* response = "{\"cmd\":\"try_match\"}\n";
                                    send(fd, response, strlen(response), 0);
                                    break;              
                                }
                                case SEND:{
                                    if(User_Info[fd].state != MATCHED){
                                        //fprintf(stderr, "Not Matched But send message\n");
                                        continue;
                                    }
                                    send(fd, BUFFER[fd], strlen(BUFFER[fd]), 0);
                                    int ClientSent = User_Info[fd].matchFd;
                                    //printf("ClientSent: %d\n",ClientSent);
                                    send(ClientSent, MessageForClient, strlen(MessageForClient), 0);
                                    break;
                                }
                                case QUIT:{
                                    send(fd, BUFFER[fd], strlen(BUFFER[fd]), 0);
                                    //printf("%d quit\n", fd);
                                    if(User_Info[fd].state == MATCHING){
                                        int command = QUIT;
                                        write(OutToChild, &command, sizeof(int));
                                        write(OutToChild, &fd, sizeof(int));
                                    }
                                    else if(User_Info[fd].state == MATCHED){
                                        TellClientQuit(User_Info[fd].matchFd);
                                        User_Info[User_Info[fd].matchFd].state = ONLINE;
                                        //printf("%d quit\n", User_Info[fd].matchFd);
                                    }
                                    User_Info[fd].state = ONLINE;
                                    break;          
                                }
                                default:{
                                    break;
                                }
                            }
                            memset(BUFFER[fd], 0, MAX_JSON);
                            CutPoint = pos + 1;
                        }
                    }
                    if(CutPoint < sz)
                        strcat(BUFFER[fd], &buffer[CutPoint]);
                    /* 進行資料處理...略...   */
                }
            }
        }
    }
    // 結束程式前記得關閉 sockfd
    close(sockfd);
}
