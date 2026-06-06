#!/usr/bin/env python3
# Licensed under Sovereign Individual License v1.0 — see LICENSE file
"""
WaveShare 1.75 I2C GPS Interface Script
======================================

This script reads GPS data from the LC76G GNSS module on the WaveShare 1.75 display
via I2C interface. It extracts GPS information in a machine-readable format.

Hardware Configuration:
- I2C bus: GPIO15 (SDA) / GPIO14 (SCL)
- Write address: 0x50 (7-bit) - for sending CASIC commands
- Read address:  0x54 (7-bit) - for receiving NMEA data
- Clock: 100 kHz

The script implements the CASIC I2C protocol as described in the Quectel I2C Application Note.
"""

import smbus2
import time
import struct
import re
from datetime import datetime

class WaveShareGPSReader:
    def __init__(self, bus_number=1, write_address=0x50, read_address=0x54):
        """
        Initialize the WaveShare GPS reader
        
        Args:
            bus_number (int): I2C bus number (default: 1)
            write_address (int): I2C write address (default: 0x50)
            read_address (int): I2C read address (default: 0x54)
        """
        self.bus = smbus2.SMBus(bus_number)
        self.write_address = write_address
        self.read_address = read_address
        self.gps_data = {
            'latitude': 0.0,
            'longitude': 0.0,
            'altitude': 0.0,
            'speed': 0.0,
            'heading': 0.0,
            'satellites': 0,
            'fix_quality': 0,
            'hdop': 0.0,
            'timestamp': None,
            'fix_valid': False,
            'visible_sats': 0
        }
        
    def _query_data_length(self):
        """
        Query the data length from the GPS module
        
        Returns:
            int: Number of available bytes or -1 on error
        """
        try:
            # CASIC command query: {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00}
            query = [0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00]
            
            # Send query to write address
            self.bus.write_i2c_block_data(self.write_address, 0, query)
            
            # Read 4 bytes from read address
            data = self.bus.read_i2c_block_data(self.read_address, 0, 4)
            
            # Convert to little-endian uint32
            length = struct.unpack('<I', bytes(data))[0]
            return length
            
        except Exception as e:
            print(f"Error querying data length: {e}")
            return -1
    
    def _request_nmea_data(self, length):
        """
        Request NMEA data from the GPS module
        
        Args:
            length (int): Number of bytes to read
            
        Returns:
            bytes: Raw NMEA data or None on error
        """
        try:
            # CASIC command: {0x00, 0x20, 0x51, 0xAA, <4 bytes length>}
            # The 4 bytes of length are in little-endian format
            length_bytes = struct.pack('<I', length)
            command = [0x00, 0x20, 0x51, 0xAA] + list(length_bytes)
            
            # Send command to write address
            self.bus.write_i2c_block_data(self.write_address, 0, command)
            
            # Read the NMEA data
            data = self.bus.read_i2c_block_data(self.read_address, 0, length)
            return bytes(data)
            
        except Exception as e:
            print(f"Error requesting NMEA data: {e}")
            return None
    
    def _parse_nmea_sentence(self, sentence):
        """
        Parse a single NMEA sentence
        
        Args:
            sentence (str): NMEA sentence to parse
        """
        if not sentence or not sentence.startswith('$'):
            return
            
        try:
            # Split by comma to get fields
            fields = sentence.split(',')
            
            # Parse GPRMC sentence (Position, time, status, etc.)
            if sentence.startswith('$GPRMC') or sentence.startswith('$GNRMC'):
                self._parse_rmc(fields)
            
            # Parse GPGGA sentence (Fix quality, altitude, satellites, etc.)
            elif sentence.startswith('$GPGGA') or sentence.startswith('$GNGGA'):
                self._parse_gga(fields)
                
            # Parse GPGSV sentence (Satellite info)
            elif sentence.startswith('$GPGSV') or sentence.startswith('$GNGSV'):
                self._parse_gsv(fields)
                
        except Exception as e:
            print(f"Error parsing NMEA sentence: {e}")
    
    def _parse_rmc(self, fields):
        """
        Parse GPRMC sentence fields
        
        Args:
            fields (list): Comma-separated fields from GPRMC sentence
        """
        try:
            # Field 1: UTC time (hhmmss.ss)
            if len(fields) > 1 and fields[1]:
                time_str = fields[1]
                if len(time_str) >= 6:
                    hour = int(time_str[0:2])
                    minute = int(time_str[2:4])
                    second = int(time_str[4:6])
                    self.gps_data['timestamp'] = datetime.now().replace(
                        hour=hour, minute=minute, second=second, microsecond=0
                    )
            
            # Field 2: Status (A=active/valid, V=void)
            if len(fields) > 2 and fields[2]:
                self.gps_data['fix_valid'] = (fields[2] == 'A')
            
            # Field 3-4: Latitude + N/S
            if len(fields) > 3 and fields[3]:
                lat_str = fields[3]
                lat_dir = fields[4] if len(fields) > 4 else ''
                if lat_str:
                    self.gps_data['latitude'] = self._nmea_to_degrees(lat_str, lat_dir)
            
            # Field 5-6: Longitude + E/W
            if len(fields) > 5 and fields[5]:
                lon_str = fields[5]
                lon_dir = fields[6] if len(fields) > 6 else ''
                if lon_str:
                    self.gps_data['longitude'] = self._nmea_to_degrees(lon_str, lon_dir)
            
            # Field 7: Speed over ground (knots → km/h)
            if len(fields) > 7 and fields[7]:
                speed_knots = float(fields[7]) if fields[7] else 0.0
                self.gps_data['speed'] = speed_knots * 1.852  # knots to km/h
            
            # Field 8: Track angle / heading (degrees true)
            if len(fields) > 8 and fields[8]:
                heading = float(fields[8]) if fields[8] else 0.0
                self.gps_data['heading'] = heading
                
        except Exception as e:
            print(f"Error parsing RMC: {e}")
    
    def _parse_gga(self, fields):
        """
        Parse GPGGA sentence fields
        
        Args:
            fields (list): Comma-separated fields from GPGGA sentence
        """
        try:
            # Field 6: Fix quality (0=invalid, 1=GPS, 2=DGPS, 6=estimated)
            if len(fields) > 6 and fields[6]:
                self.gps_data['fix_quality'] = int(fields[6])
                if self.gps_data['fix_quality'] > 0:
                    self.gps_data['fix_valid'] = True
            
            # Field 7: Satellites used
            if len(fields) > 7 and fields[7]:
                self.gps_data['satellites'] = int(fields[7])
            
            # Field 8: HDOP
            if len(fields) > 8 and fields[8]:
                self.gps_data['hdop'] = float(fields[8])
            
            # Field 9: Altitude above MSL
            if len(fields) > 9 and fields[9]:
                self.gps_data['altitude'] = float(fields[9])
                
        except Exception as e:
            print(f"Error parsing GGA: {e}")
    
    def _parse_gsv(self, fields):
        """
        Parse GPGSV sentence fields
        
        Args:
            fields (list): Comma-separated fields from GPGSV sentence
        """
        try:
            # Field 3: Total satellites in view (for this constellation)
            if len(fields) > 3 and fields[3]:
                total_sats = int(fields[3])
                self.gps_data['visible_sats'] = total_sats
                
        except Exception as e:
            print(f"Error parsing GSV: {e}")
    
    def _nmea_to_degrees(self, val, dir):
        """
        Convert NMEA latitude/longitude (DDmm.mmmm) to decimal degrees
        
        Args:
            val (str): NMEA coordinate value
            dir (str): Direction (N, S, E, W)
            
        Returns:
            float: Decimal degrees
        """
        try:
            if not val or val == '':
                return 0.0
            
            raw = float(val)
            degrees = int(raw / 100.0)
            minutes = raw - (degrees * 100.0)
            result = degrees + (minutes / 60.0)
            
            if dir and (dir == 'S' or dir == 'W'):
                result = -result
                
            return result
            
        except Exception as e:
            print(f"Error converting NMEA to degrees: {e}")
            return 0.0
    
    def read_gps_data(self):
        """
        Read GPS data from the LC76G module
        
        Returns:
            dict: GPS data in machine-readable format
        """
        try:
            # Query data length
            length = self._query_data_length()
            if length <= 0:
                print("No data available or error in length query")
                return self.gps_data
            
            # Request NMEA data
            data = self._request_nmea_data(length)
            if not data:
                print("Failed to read NMEA data")
                return self.gps_data
            
            # Parse NMEA sentences
            data_str = data.decode('utf-8', errors='ignore')
            sentences = data_str.split('\n')
            
            for sentence in sentences:
                sentence = sentence.strip()
                if sentence.startswith('$'):
                    self._parse_nmea_sentence(sentence)
            
            return self.gps_data
            
        except Exception as e:
            print(f"Error reading GPS data: {e}")
            return self.gps_data
    
    def get_json_data(self):
        """
        Get GPS data in JSON format
        
        Returns:
            str: JSON formatted GPS data
        """
        import json
        return json.dumps(self.gps_data, indent=2, default=str)
    
    def print_data(self):
        """
        Print GPS data in a human-readable format
        """
        data = self.read_gps_data()
        print("=== GPS Data ===")
        print(f"Latitude:  {data['latitude']:.6f}°")
        print(f"Longitude: {data['longitude']:.6f}°")
        print(f"Altitude:  {data['altitude']:.1f} m")
        print(f"Speed:     {data['speed']:.1f} km/h")
        print(f"Heading:   {data['heading']:.1f}°")
        print(f"Satellites: {data['satellites']}")
        print(f"Fix Quality: {data['fix_quality']}")
        print(f"HDOP:      {data['hdop']:.1f}")
        print(f"Fix Valid: {data['fix_valid']}")
        print(f"Visible Sats: {data['visible_sats']}")
        if data['timestamp']:
            print(f"Timestamp: {data['timestamp']}")
        print("===============")

def main():
    """
    Main function to demonstrate the GPS reader
    """
    print("WaveShare 1.75 GPS I2C Reader")
    print("=============================")
    
    # Create GPS reader instance
    gps_reader = WaveShareGPSReader()
    
    try:
        # Read and display GPS data
        gps_reader.print_data()
        
        # Get JSON data
        json_data = gps_reader.get_json_data()
        print("\nJSON Data:")
        print(json_data)
        
    except Exception as e:
        print(f"Error in main execution: {e}")

if __name__ == "__main__":
    main()