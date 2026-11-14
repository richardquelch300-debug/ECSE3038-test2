import json
import os
import secrets
from datetime import datetime, timezone
from typing import Literal, Optional

import paho.mqtt.publish as mqtt_publish
from dotenv import load_dotenv
from fastapi import FastAPI, Header, HTTPException, Query, Response
from pydantic import BaseModel, Field
from pymongo import DESCENDING, MongoClient

load_dotenv()

MONGODB_URI = os.getenv("MONGODB_URI")
MQTT_NAME = os.getenv("MQTT_NAME")
MQTT_HOST = os.getenv("MQTT_HOST", "135.232.203.144")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

client = MongoClient(MONGODB_URI)
db = client["test2"]
fans = db["fans"]
readings = db["readings"]
settings = db["settings"]

fans.create_index("mac", unique=True)
readings.create_index([("timestamp", DESCENDING)])

app = FastAPI(title="ECSE3038 Test 2: The bathroom fan")


# ---------- Models ----------

class FanIn(BaseModel):
    mac: str
    room: str


class ReadingIn(BaseModel):
    mac: str
    humidity: float = Field(ge=0, le=100)


class SettingsIn(BaseModel):
    mode: Literal["auto", "on", "off"]
    max_humidity: float = Field(ge=0, le=100)


# ---------- Helpers ----------

def public_fan(doc):
    """A fan as the outside world sees it: no _id, no key."""
    return {"mac": doc["mac"], "room": doc["room"]}


def fan_for_key(key):
    if not key:
        return None
    return fans.find_one({"key": key})


# ---------- A1: fans ----------

@app.post("/fans", status_code=201)
def create_fan(fan: FanIn):
    if fans.find_one({"mac": fan.mac}):
        raise HTTPException(status_code=409, detail="A fan with this MAC already exists")
    key = secrets.token_urlsafe(16)
    fans.insert_one({"mac": fan.mac, "room": fan.room, "key": key})
    return {"mac": fan.mac, "room": fan.room, "key": key}


@app.get("/fans")
def list_fans():
    return [public_fan(doc) for doc in fans.find({}, {"_id": 0, "key": 0})]


@app.get("/fans/{mac}")
def get_fan(mac: str):
    doc = fans.find_one({"mac": mac}, {"_id": 0, "key": 0})
    if doc is None:
        raise HTTPException(status_code=404, detail="Fan not found")
    return public_fan(doc)


# ---------- A2: post a reading ----------

@app.post("/readings", status_code=201)
def create_reading(reading: ReadingIn, x_api_key: Optional[str] = Header(default=None)):
    owner = fan_for_key(x_api_key)
    if owner is None:
        raise HTTPException(status_code=401, detail="Missing or unknown API key")
    if owner["mac"] != reading.mac:
        raise HTTPException(status_code=403, detail="This key belongs to another fan")

    doc = {
        "mac": reading.mac,
        "humidity": reading.humidity,
        "timestamp": datetime.now(timezone.utc),
    }
    readings.insert_one(doc)
    return {
        "mac": doc["mac"],
        "humidity": doc["humidity"],
        "timestamp": doc["timestamp"].isoformat(),
    }


# ---------- A3: list readings ----------

@app.get("/readings")
def list_readings(
    mac: Optional[str] = None,
    limit: int = Query(default=10, ge=1, le=100),
):
    query = {"mac": mac} if mac else {}
    cursor = readings.find(query, {"_id": 0}).sort("timestamp", DESCENDING).limit(limit)
    result = []
    for doc in cursor:
        ts = doc["timestamp"]
        if ts.tzinfo is None:
            ts = ts.replace(tzinfo=timezone.utc)
        result.append({"mac": doc["mac"], "humidity": doc["humidity"], "timestamp": ts.isoformat()})
    return result


# ---------- A4: fan settings ----------

@app.put("/fans/{mac}/settings")
def put_settings(mac: str, body: SettingsIn, response: Response):
    if fans.find_one({"mac": mac}) is None:
        raise HTTPException(status_code=404, detail="Fan not found")

    new_settings = {"mode": body.mode, "max_humidity": float(body.max_humidity)}
    result = settings.replace_one({"mac": mac}, {"mac": mac, **new_settings}, upsert=True)
    response.status_code = 201 if result.upserted_id is not None else 200

    mqtt_publish.single(
        f"ecse3038/{MQTT_NAME}/fan/settings",
        payload=json.dumps(new_settings),
        qos=1,
        retain=True,
        hostname=MQTT_HOST,
        port=MQTT_PORT,
        auth={"username": MQTT_NAME},
    )


# ---------- A5: delete a fan ----------

@app.delete("/fans/{mac}", status_code=204)
def delete_fan(mac: str, x_api_key: Optional[str] = Header(default=None)):
    if fans.find_one({"mac": mac}) is None:
        raise HTTPException(status_code=404, detail="Fan not found")
    owner = fan_for_key(x_api_key)
    if owner is None:
        raise HTTPException(status_code=401, detail="Missing or unknown API key")
    if owner["mac"] != mac:
        raise HTTPException(status_code=403, detail="This key belongs to another fan")

    fans.delete_one({"mac": mac})
    settings.delete_one({"mac": mac})
    return Response(status_code=204)
