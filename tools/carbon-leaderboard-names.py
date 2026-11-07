#!/usr/bin/env python3
"""carbon-leaderboard-names.py: the names file the live leaderboards page needs.

The server's GET /leaderboard sends race hashes ("30F07253") and car keys ("0x8774322c"), never
names. This writes names.json for the page: per race hash its race id, name, track, type, group
and laps (from the game data that logs/leaderboards-preview/race-names.py read), and per car key
its display name. Run again when race-names.json or car-ids.json change.

Usage: python3 tools/carbon-leaderboard-names.py [DATA_DIR] [OUT]
  DATA_DIR  default logs/leaderboards-preview/data (race-names.json, car-ids.json)
  OUT       default logs/leaderboard-server-20261106/page/names.json
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "logs/leaderboards-preview/data")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "logs/leaderboard-server-20261106/page/names.json")

# The same names as logs/leaderboards-preview/build-data.py.
CAR_NAMES = {"240SX": "Nissan 240SX", "300C": "Chrysler 300C SRT8", "350Z": "Nissan 350Z", "911TURBO": "Porsche 911 Turbo", "997GT3RS": "Porsche 911 GT3 RS", "997S": "Porsche 911 Carrera S", "997TT": "Porsche 911 Turbo (997)", "A4": "Audi A4 3.2 quattro", "BMWM3GTRE46": "BMW M3 GTR", "BRERA": "Alfa Romeo Brera", "CAMARO": "Chevrolet Camaro SS", "CAMARON": "Chevrolet Camaro Concept", "CARRERAGT": "Porsche Carrera GT", "CAYMANS": "Porsche Cayman S", "CCX": "Koenigsegg CCX", "CHALLENGER71": "Dodge Challenger R/T", "CHALLENGERN": "Dodge Challenger Concept", "CHARGER06": "Dodge Charger SRT8", "CHARGER69": "Dodge Charger R/T", "CHEVELLE": "Chevrolet Chevelle SS", "CLIO": "Renault Clio V6", "CLK500": "Mercedes-Benz CLK 500", "COROLLA": "Toyota Corolla GTS", "CORVETTEZ06": "Chevrolet Corvette Z06", "CTS": "Cadillac CTS", "CUDA": "Plymouth Hemi 'Cuda", "DB9": "Aston Martin DB9", "ECLIPSE": "Mitsubishi Eclipse", "ECLIPSEGT": "Mitsubishi Eclipse GT", "ELISE": "Lotus Elise", "EUROPA": "Lotus Europa S", "FORDGT": "Ford GT", "G35": "Infiniti G35", "GALLARDO": "Lamborghini Gallardo", "GTI": "Volkswagen Golf GTI", "GTO": "Pontiac GTO", "IMPREZAWRXSTI": "Subaru Impreza WRX STI", "IS300": "Lexus IS 300", "JAGUARXK": "Jaguar XK", "LANCEREVO9": "Mitsubishi Lancer EVO IX", "MAZDA3SPORT": "Mazda 3", "MAZDASPEED3": "Mazdaspeed3", "MONARO": "Vauxhall Monaro VXR", "MR2": "Toyota MR2", "MURCIELAGO": "Lamborghini Murciélago", "MURCIELAGO640": "Lamborghini Murciélago LP640", "MUSTANGGT": "Ford Mustang GT", "MUSTANGSHLBYN": "Ford Shelby GT500", "MUSTANGSHLBYO": "Ford Shelby GT500 (1967)", "R32": "Volkswagen R32", "R8": "Audi R8", "ROADRUNNER": "Plymouth Road Runner", "RX7": "Mazda RX-7", "RX8": "Mazda RX-8", "SKYLINE": "Nissan Skyline GT-R", "SL65": "Mercedes-Benz SL 65 AMG", "SLR": "Mercedes-Benz SLR McLaren", "SUPRA": "Toyota Supra", "VIPER": "Dodge Viper SRT-10", "ZONDA": "Pagani Zonda F", "COPSPORT": "Police Corvette", "COPMIDSIZE": "Police cruiser", "COPGTO": "Police GTO", "COPSUV": "Police SUV"}


def main():
    races = {}
    for rid, r in json.load(open(os.path.join(DATA, "race-names.json"))).items():
        races[r["hash"]] = {"race": rid, **{k: r.get(k) for k in ("name", "track", "type", "group", "laps")}}
    cars = {}
    for key, folder in json.load(open(os.path.join(DATA, "car-ids.json"))).items():
        cars[key.lower()] = CAR_NAMES.get(folder, folder.title())
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        json.dump({"races": races, "cars": cars}, f, separators=(",", ":"), ensure_ascii=False)
    print(f"{OUT}: {len(races)} races, {len(cars)} cars")


if __name__ == "__main__":
    main()
