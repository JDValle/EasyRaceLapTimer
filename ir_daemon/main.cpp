/**
 * EasyRaceLapTimer - Copyright 2015-2016 by airbirds.de, a project of polyvision UG (haftungsbeschränkt)
 *
 * Author: Alexander B. Bierbrauer
 *
 * This file is part of EasyRaceLapTimer.
 *
 * OpenRaceLapTimer is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 * OpenRaceLapTimer is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with Foobar. If not, see http://www.gnu.org/licenses/.
 **/

#include <QtCore/QCoreApplication>
#include <stdio.h>
#include <QProcess>
#include <QTextStream>
#include <QDebug>
#include <QtConcurrent>
#include <curl/curl.h>
#include "restart_button_input.h"
#include "buzzer.h"

#define VERSION "0.3"

#ifndef __APPLE__
    #include <wiringPi.h>
#else
    #define millis() (0)
    #define micros() (0)
#endif

#define BIT_SET(a,b) ((a) |= (1<<(b)))
#define BIT_CLEAR(a,b) ((a) &= ~(1<<(b)))
#define BIT_FLIP(a,b) ((a) ^= (1<<(b)))
#define BIT_CHECK(a,b) ((a) & (1<<(b)))

#define	IR_LED_1	1
#define	IR_LED_2	4
#define	IR_LED_3	5
#define BUZZER_PIN	6
#define RESTART_BUTTON_PIN 14

#define PULSE_ONE	500
#define PULSE_MIN 100
#define PULSE_MAX 1000

#define DATA_BIT_LENGTH 9
#define BUZZER_ACTIVE_TIME_IN_MS	40

int sensor_state[3];
unsigned int sensor_pulse[3];
unsigned int sensor_start_lap_time[3];
QList<int> sensor_data[3];

bool debug_mode = false;
RestartButtonInput *pRestartButton = new RestartButtonInput(RESTART_BUTTON_PIN);

unsigned int num_ones_in_buffer(QList<int>& list){
	unsigned int t= 0;
	for(int i=0; i < list.length(); i++){ // -1 because we don't count the last bit.. it's the checksum bit!
		if(list[i] == 1){
			t += 1;
		}
	}
	return t;
}

void post_request(int token,unsigned int lap_time){
    CURL *curl;
    CURLcode res;

    curl = curl_easy_init();
      if(curl) {
        /* First set the URL that is about to receive our POST. This URL can
           just as well be a https:// URL if that is what should receive the
           data. */
        curl_easy_setopt(curl, CURLOPT_URL, QString("http://localhost/api/v1/lap_track/create?transponder_token=%1&lap_time_in_ms=%2").arg(token).arg(lap_time).toStdString().c_str());



        /* Perform the request, res will get the return code */
        res = curl_easy_perform(curl);
        /* Check for errors */
        if(res != CURLE_OK){
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }


        /* always cleanup */
        curl_easy_cleanup(curl);
      }
}

void print_binary_list(QList<int>& list){
	for(int i=0; i < list.length(); i++){
		if(list[i] == 1){
			printf("1");
		}else{
			printf("0");
		}
	}
	printf("\n");
}

void push_to_service(int sensor_i,QList<int>& list,unsigned int delta_time,int control_bit){
	unsigned int val_to_push = 0;
	print_binary_list(list);
	
	list.removeFirst();
	list.removeFirst();
	list.removeLast();
	//printf("list length: %i\n",list.length());
	for(int i=0; i < list.length(); i++){
		if(list[i] == 1){
			BIT_SET(val_to_push,list.length() - i -1);
		}else{
			BIT_CLEAR(val_to_push,list.length() -i - 1 );
		}
	}
	
	//printf("after: ");
	//print_binary_list(list);
	//qDebug() << "result binary:" << QString::number(val_to_push,2) << "\n";
	//qDebug() << "ones in buffer " << num_ones_in_buffer(list) << "\n";
	int own_control_bit = (int)num_ones_in_buffer(list) % 2;
	
	if(control_bit == own_control_bit){
		printf("sensor: %i token: %u time: %u\n",sensor_i,val_to_push,delta_time);
		Buzzer::instance()->activate(BUZZER_ACTIVE_TIME_IN_MS);
		QtConcurrent::run(post_request,val_to_push,delta_time);
        //post_request(val_to_push,delta_time); // this sends the request to the rails web app
	}else{
		printf("sensor: %i control bit wrong: %i own_control_bit: %i, token: %u\n",sensor_i,control_bit,own_control_bit,val_to_push);
	}
}

void push_bit_to_sensor_data(unsigned int pulse_width,int sensor_i){
	if(pulse_width >= PULSE_ONE){
		
		sensor_data[sensor_i] << 1;
		//printf("ONE %i\n",pulse_width);
	}else{
		
		//printf("ZERO %i\n",pulse_width);
		sensor_data[sensor_i] << 0;
	}
	
	if(sensor_data[sensor_i].length() == DATA_BIT_LENGTH){
		// first two bytes have to be zero
		if(sensor_data[sensor_i][0] == 0 && sensor_data[sensor_i][1] == 0){
			//print_binary_list(sensor_data[sensor_i]);
			
			
			// check if there's a tracked time for the current sensor_data
			// if yes, push it
			if(sensor_start_lap_time[sensor_i] != 0)
			{
				unsigned int diff = millis() - sensor_start_lap_time[sensor_i];
				if(diff > 1000){ // only push if there's difference of 2 seconds between tracking
					push_to_service(sensor_i,sensor_data[sensor_i],diff,sensor_data[sensor_i].last());
					sensor_start_lap_time[sensor_i] = 0;
				}
			}
			else
			{
				sensor_start_lap_time[sensor_i] = millis();
			}
			
			sensor_data[sensor_i].clear();
		}else{
			sensor_data[sensor_i].removeFirst();
		}
	}
}

int main(int argc, char *argv[])
{
    QTextStream qout(stdout);
    QCoreApplication a(argc, argv);
    curl_global_init(CURL_GLOBAL_ALL);

	
    printf("starting ir_daemon v%s\n",VERSION);

	if(a.arguments().count() > 1){
		if(a.arguments().at(1).compare("--debug") == 0){
			debug_mode = true;
			printf("enabled debug mode\n");
		}
	}

	wiringPiSetup () ;
    pinMode(IR_LED_1,INPUT);
	pinMode(IR_LED_2,INPUT);
	pinMode(IR_LED_3,INPUT);
	
	pinMode(BUZZER_PIN,OUTPUT);
	Buzzer::instance()->setPin(BUZZER_PIN);
	digitalWrite(BUZZER_PIN,LOW);


	for(int sensor_i = 0; sensor_i < 3; sensor_i++){
		sensor_state[sensor_i] = 0;
		sensor_pulse[sensor_i] = 0;
		sensor_start_lap_time[sensor_i] = 0;
	}
	
	while(1){
		for(int sensor_i = 0; sensor_i < 3; sensor_i++){
			
			int pid_input = IR_LED_1;
			switch(sensor_i){
				case 0:
					pid_input = IR_LED_1;
					break;
				case 1:
					pid_input = IR_LED_2;
					break;
				case 2:
					pid_input = IR_LED_3;
					break;
			}
			
            int state = digitalRead(pid_input);

			if(state != sensor_state[sensor_i]){
				sensor_state[sensor_i] = state;
				unsigned int c_time = micros();
				unsigned int c_pulse = c_time - sensor_pulse[sensor_i];
				
				if(debug_mode){
					printf("sensor %i: pulse %i\n",sensor_i,c_pulse);
				}
				if(c_pulse >= PULSE_MIN && c_pulse <= PULSE_MAX){
					push_bit_to_sensor_data(c_pulse,sensor_i);
				}else{
					sensor_data[sensor_i].clear();
				}
				sensor_pulse[sensor_i] = c_time;
			}
		}
		
		
		Buzzer::instance()->update();
		pRestartButton->update();
	}

    curl_global_cleanup();
    return a.exec();
}
