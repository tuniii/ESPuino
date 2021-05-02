#!/usr/bin/python
from zeroconf import ServiceBrowser, Zeroconf
from typing import cast
from time import sleep
import sys
import requests
import argparse

class EspuinoListener:
    def __init__(self):
        self.ip_address = None

    def add_service(self, zeroconf, type, name):
        info = zeroconf.get_service_info(type, name)
        if self.ip_address == None:
            self.ip_address = info.parsed_addresses()[0]
    
    def get_ip_address(self):
        return self.ip_address

def send_firmware(file, address):
    with open(file, 'rb') as f:
        print('Updating firmware...')
        url = 'http://' + address + '/update'
        response = requests.post(url, files={'firmware.bin': f})
        if response.status_code == 200:
            print('Restart system...')
            url = 'http://' + address + '/restart'
            response = requests.get(url)

def find_device():
    zeroconf = Zeroconf()
    listener = EspuinoListener()
    browser = ServiceBrowser(zeroconf, '_espuino._tcp.local.', listener)

    while listener.get_ip_address() == None:
        sleep(0.05)
    
    return listener.get_ip_address()

if __name__ == '__main__':
    send_firmware(sys.argv[1], find_device())