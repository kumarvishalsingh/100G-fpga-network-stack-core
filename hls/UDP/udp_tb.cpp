/**********
Copyright (c) 2020, Xilinx, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********/

#include "udp.hpp"
#include "../TOE/testbench/pcap2stream.hpp"
#include <string.h>

socket_table            SocketTableRx[NUMBER_SOCKETS];
socket_table            SocketTableTx[NUMBER_SOCKETS];
stream<axiWordUdp>      DataInApp("DataInApp");
stream<axiWordUdp>      AppRxGolden("AppRxGolden");

#include <fstream>
#include <iterator>
#include <vector>

void readTxtFile () {
    std::ifstream input("shakespeare.txt", std::ios::binary);

    std::vector<char> text(
         (std::istreambuf_iterator<char>(input)),
         (std::istreambuf_iterator<char>())
    );
    input.close();

    /*Create payloads ranging from 1 to 1508 bytes */
    for (unsigned int upper_limit= 1; upper_limit <(text.size() + 1); upper_limit++){
        axiWordUdp currWord = axiWordUdp(0,0,1,0);
        for(unsigned int i = 0; i < upper_limit; i++) {
            unsigned int lb = (i % 64) * 8;
            currWord.data(lb + 7 , lb) = text.at(i);
            currWord.keep.bit(lb/8)    = 1;
            //std::cout << "upper_limit: " << std::setw(3) << upper_limit << "\ti: " << std::setw(3) << i << "\tlb: " << std::setw(2) << lb << "\tvector: " << (char) text.at(i) << std::endl;
            if ((i + 1) == upper_limit) {
                currWord.last = 1;
                DataInApp.write(currWord);
                AppRxGolden.write(currWord);
                currWord.user = 0;
            }
            else if (((i + 1) % 64) == 0){
                DataInApp.write(currWord);
                AppRxGolden.write(currWord);
                currWord = axiWordUdp(0,0,1,0);
            }
        }
    }
}

void fillTables(){
    // Initialize tables to 0
    for (unsigned int i=0; i < NUMBER_SOCKETS; i++){
        SocketTableRx[i]= socket_table(0,0,0,0);
    }

    SocketTableRx[ 1] = socket_table(0xC0A8000A,53211,60279,1);
    SocketTableRx[ 6] = socket_table(0xC0A80017,58517,60280,1);
    SocketTableRx[11] = socket_table(0xC0A80085,54591,60281,1);

    memcpy(SocketTableTx, SocketTableRx, sizeof(socket_table) * NUMBER_SOCKETS);
#ifdef PRINT_TABLE
    for (unsigned int m=0; m< NUMBER_SOCKETS; m++){
        std::cout<< "TxTable["<< std::setw(2) << m << "] theirIP: " << SocketTableTx[m].theirIP << "\ttheirPort: " << SocketTableTx[m].theirIP;
        std::cout<< "\tmyPort: " << SocketTableTx[m].myPort << "\tvalid: " << SocketTableTx[m].valid <<std::endl;
    }
#endif    
}


int main(void){

    stream<axiWord>         rxUdpDataIn("rxUdpDataIn");
    stream<axiWord>         txUdpDataOut("txUdpDataOut");
    stream<axiWord>         txUdpDataOutpcap("txUdpDataOutpcap");
    stream<axiWord>         txUdpDataOutComp("txUdpDataOutComp");
    stream<axiWord>         streamGoldenData("streamGoldenData");
    stream<axiWord>         txGoldenData("txGoldenData");
    stream<axiWordUdp>      DataOutApp("DataOutApp");
    ap_uint<32>             myIpAddress = byteSwap<32>(0xC0A80005);
    ap_uint<16>             numberSockets;
    stream<axiWordUdp>      DataInAppBK("DataInAppBK");
    char *outputPcap = (char *) "txPathOutputSim.pcap";
    char *goldenDataPtr = (char *) "goldenDataTx.pcap";


    fillTables();
    readTxtFile();

    // read pcap golden data into a stream
    pcap2stream(goldenDataPtr, false, streamGoldenData);

    // Duplicate pcap data, one stream is used to feed the rxUdpDataIn and the other as golden output for the Tx Side
    unsigned int rxBeats = 0;
    while(!streamGoldenData.empty()){
        axiWord currWord,sendWord;
        streamGoldenData.read(currWord);
        sendWord = currWord;
        // Swap IPs and Port to make it match table
        if (rxBeats == 0){
            ap_uint<32> auxIP;
            ap_uint<16> auxPort;
            auxIP   = sendWord.data(127, 96);
            auxPort = sendWord.data(175,160);
            sendWord.data(127, 96) = sendWord.data(159,128);
            sendWord.data(159,128) = auxIP;
            sendWord.data(175,160) = sendWord.data(191,176);
            sendWord.data(191,176) = auxPort;
        }
        rxBeats++;
        if (currWord.last) 
            rxBeats = 0;

        rxUdpDataIn.write(sendWord);
        txGoldenData.write(currWord);
    }

    for (unsigned int cycle_count = 0; cycle_count < 50000 ; cycle_count ++) {
        udp(
            rxUdpDataIn,
            txUdpDataOut,
            DataOutApp,  
            DataInApp,
            myIpAddress,
            SocketTableRx,
            SocketTableTx,
            numberSockets);
    }

    /*************************************
      Check Rx output against golden data
     *************************************/ 
    unsigned int rxPacket = 0;
    unsigned int rxBeat   = 0;
    int          rxErrors = 0;

    while(!AppRxGolden.empty()){
        axiWordUdp goldenWord, currWord;
        AppRxGolden.read(goldenWord);
        DataOutApp.read(currWord);
        if (goldenWord.data != currWord.data){
#ifndef DEBUG
            std::cerr << "Rx path, packet[" << std::setw(4) << rxPacket << "][" << std::setw(2);
            std::cerr << rxBeat << "] data field does not match golden data" << std::endl;
#endif
            rxErrors--;
        }

        if (goldenWord.keep != currWord.keep){
#ifndef DEBUG
            std::cerr << "Rx path, packet[" << std::setw(4) << rxPacket << "][" << std::setw(2);
            std::cerr << rxBeat << "] keep field does not match golden data" << std::endl;
#endif
            rxErrors--;
        }
        if (goldenWord.user != currWord.user){
#ifndef DEBUG
            std::cerr << "Rx path, packet[" << std::setw(4) << rxPacket << "][" << std::setw(2);
            std::cerr << rxBeat << "] user field does not match golden data" << std::endl;
#endif
            rxErrors--;
        }        
        if (goldenWord.last != currWord.last){
#ifndef DEBUG
            std::cerr << "Rx path, packet[" << std::setw(4) << rxPacket << "][" << std::setw(2);
            std::cerr << rxBeat << "] last field does not match golden data" << std::endl;
#endif
            rxErrors--;
        }
        rxBeat++;
        if (goldenWord.last){
            rxPacket++;
            rxBeat = 0;
        }
    }

    /*************************************
      Check Tx output against golden data
     *************************************/ 
    /*Duplicate tx stream to save it as pcap file and to compare*/
    while(!txUdpDataOut.empty()){
        axiWord currWordT;
        txUdpDataOut.read(currWordT);
        txUdpDataOutpcap.write(currWordT);
        txUdpDataOutComp.write(currWordT);
    }

    /*Store Tx path in a pcap file*/
    stream2pcap(outputPcap,false,false,txUdpDataOutpcap,true);

    unsigned int txPacket = 0;
    unsigned int txBeat = 0;
    int          txErrors = 0;

    while(!txGoldenData.empty()){
        axiWord currWordS, currWordG;
        txUdpDataOutComp.read(currWordS);
        txGoldenData.read(currWordG);
        
        if (currWordS.data != currWordG.data){
#ifdef DEBUG
            std::cerr << "Tx path, packet[" << std::setw(4) << txPacket << "][" << std::setw(2);
            std::cerr << txBeat << "] data field does not match golden data" << std::endl;
#endif
            txErrors--;
        }

        if (currWordS.keep != currWordG.keep){
#ifdef DEBUG
            std::cerr << "Tx path, packet[" << std::setw(4) << txPacket << "][" << std::setw(2);
            std::cerr << txBeat << "] keep field does not match golden data" << std::endl;
#endif
            txErrors--;
        }
        if (currWordS.last != currWordG.last){
#ifdef DEBUG
            std::cerr << "Tx path, packet[" << std::setw(4) << txPacket << "][" << std::setw(2);
            std::cerr << txBeat << "] last field does not match golden data" << std::endl;
#endif
            txErrors--;
        }

        txBeat++;
        if (currWordG.last){
            txPacket++;
            txBeat = 0;
        }
    }

    /*************************************
             Check for errors
     *************************************/ 
    if (rxErrors == 0)
        std::cout << "Rx path test PASSED, total packets " << std::dec << rxPacket << std::endl;

    if (txErrors == 0)
        std::cout << "Tx path test PASSED, total packets " << std::dec << txPacket << std::endl;


    if ((txErrors - rxErrors) != 0){
        std::cerr << "One of both test not PASSED, run the simulation with DEBUG enabled to see errors" << std::endl;
        return (txErrors + rxErrors);
    }
    else
        return 0;



}
