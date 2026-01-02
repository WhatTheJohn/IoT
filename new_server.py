import socketio
import uvicorn
import numpy as np
import requests
import time
from collections import deque
from fastapi import FastAPI

# --- CONFIGURATION ---
# Using the key you provided. If it fails, it falls back to "Mock" mode automatically.
OPENWEATHER_API_KEY = "" 
CITY = "Kuala Lumpur" 
WEATHER_API_URL = f"http://api.openweathermap.org/data/2.5/weather?q={CITY}&appid={OPENWEATHER_API_KEY}&units=metric"

# --- SETUP FASTAPI & SOCKET.IO ---
sio = socketio.AsyncServer(async_mode='asgi', cors_allowed_origins='*')
app = FastAPI()
sio_app = socketio.ASGIApp(sio, app)

# --- GLOBAL STATE ---
class PlantBrain:
    def __init__(self):
        # 1. Data Cleaning Buffers (Window size = 5)
        self.soil_buffer = deque(maxlen=5)
        self.temp_buffer = deque(maxlen=5)
        self.light_buffer = deque(maxlen=5)
        
        # 2. Last Sent Values (For Delta Compression Logic)
        self.last_sent_temp = -999.0
        self.last_sent_soil = -999.0
        
        # 3. System State
        self.pump_status = False
        self.pump_lock = False # Safety Lock
        self.last_pump_time = 0
        self.alert_message = "System Normal"
        self.battery_level = 100 # Simulating battery
        
        # 4. External Data
        self.weather_condition = "Unknown" 
        self.outside_temp = 30.0
        self.outside_humidity = 70.0 
        self.last_weather_fetch = 0

    def get_weather(self):
        """Fetches external weather every 10 minutes"""
        now = time.time()
        if now - self.last_weather_fetch < 600: return

        try:
            r = requests.get(WEATHER_API_URL)
            data = r.json()
            if data.get("cod") == 200:
                self.weather_condition = data['weather'][0]['main']
                self.outside_temp = data['main']['temp']
                self.outside_humidity = data['main']['humidity']
                print(f"☁️ Weather Updated: {self.weather_condition}, {self.outside_temp}°C")
        except:
            self.weather_condition = "Clouds" 
            print("⚠️ Weather API unavailable, using Mock data.")
        
        self.last_weather_fetch = now

    def calculate_vpd(self, temp, humidity):
        """Calculates Vapor Pressure Deficit"""
        svp = 0.6108 * np.exp((17.27 * temp) / (temp + 237.3))
        avp = svp * (humidity / 100.0)
        return svp - avp

plant = PlantBrain()

# --- SOCKET EVENTS ---

@sio.event
async def connect(sid, environ):
    print(f"✅ Client Connected: {sid}")

@sio.event
async def sensor_data(sid, data):
    """
    Main Logic Loop
    """
    # --- STEP 1: DATA CLEANING (PDF Pg 32) ---
    plant.soil_buffer.append(data['soil_moisture'])
    plant.temp_buffer.append(data['temperature'])
    plant.light_buffer.append(data['light_intensity'])
    
    # Wait for buffer to fill slightly
    if len(plant.soil_buffer) < 2: return

    # Moving Average Algorithm
    avg_soil = sum(plant.soil_buffer) / len(plant.soil_buffer)
    avg_temp = sum(plant.temp_buffer) / len(plant.temp_buffer)
    avg_light = sum(plant.light_buffer) / len(plant.light_buffer)

    plant.last_sent_temp = avg_temp
    plant.last_sent_soil = avg_soil

    # --- STEP 2: CONTEXT ---
    plant.get_weather()
    
    # --- STEP 3: HYBRID DECISION MAKING (PDF Part 2) ---
    decision = "Optimal"
    pump_command = False
    
    # 3.1 Safety Layer (Deterministic Rules)
    if avg_soil > 80:
        plant.pump_lock = True
        decision = "Saturation Lock"
        plant.alert_message = "Soil too wet! Pump disabled."
    
    elif avg_temp > 35:
        plant.pump_lock = True
        decision = "Heat Stress"
        plant.alert_message = "High Temp! Watering paused."
        
    elif data['nitrogen'] > 200 or data['potassium'] > 200:
         plant.pump_lock = True
         decision = "Salinity Risk"
         plant.alert_message = "Nutrient Burn Risk! Flush soil."
         
    else:
        plant.pump_lock = False
        plant.alert_message = "System Normal"

    # 3.2 Intelligence Layer (VPD Logic)
    if not plant.pump_lock:
        
        vpd = plant.calculate_vpd(avg_temp, plant.outside_humidity)
        watering_threshold = 40.0 # Default
        
        # Adjust based on Environment
        if plant.weather_condition == "Rain":
            watering_threshold = 20.0 
            decision = "Raining (Passive)"
        elif vpd > 1.2: 
            watering_threshold = 50.0 # Air is dry, water sooner
            decision = "High Transpiration"
            
        # Critical State Check
        if avg_soil < watering_threshold:
            pump_command = True
            decision = "Critical: Pulse Irrigation"
            
            # Pulse Logic (Max 10s run)
            if not plant.pump_status: plant.last_pump_time = time.time()
            if time.time() - plant.last_pump_time > 10:
                pump_command = False
                plant.alert_message = "Pump Timeout (Safety)"

    plant.pump_status = pump_command

    # --- STEP 4: OUTPUT ---
    output_payload = {
        "timestamp": time.strftime("%H:%M:%S"),
        "sensors": {
            "soil": round(avg_soil, 1),
            "temp": round(avg_temp, 1),
            "light": round(avg_light, 1),
            "npk": f"{data['nitrogen']}-{data['phosphorus']}-{data['potassium']}"
        },
        "weather": {
            "condition": plant.weather_condition,
            "temp": plant.outside_temp,
            "humidity": plant.outside_humidity,
            "vpd": round(vpd if 'vpd' in locals() else 0, 2)
        },
        "system": {
            "pump_active": plant.pump_status,
            "algorithm_state": decision,
            "alert": plant.alert_message,
            "battery_level": 85 # Simulated
        }
    }
    
    await sio.emit('system_update', output_payload)

@sio.event
async def manual_control(sid, data):
    """Handle manual button press from App"""
    if data['action'] == 'pump_on':
        print("Manual Pump Activation")
        # In a real system, this would override logic for 5s
        pass

@sio.event
async def disconnect(sid):
    print(f"❌ Client Disconnected: {sid}")

if __name__ == '__main__':
    # Running on Port 5001 to avoid AirPlay conflict
    uvicorn.run(sio_app, host='0.0.0.0', port=5001)
