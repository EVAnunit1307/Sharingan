#!/usr/bin/env python3
"""Standalone LD2450 dashboard. Use only when the combined dashboard is stopped."""
import argparse
from pathlib import Path
import signal
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'CV'))
from flask import Flask, jsonify, redirect
from radar_service import CONFIG_PATH, RadarService
from radar_web import register_radar_routes


def create_app(radar):
    app = Flask(__name__)
    register_radar_routes(app, radar)

    @app.get('/')
    def index():
        return redirect('/radar')

    @app.get('/targets')
    def compatibility():
        state = radar.snapshot()
        return jsonify(ok=state['status'] == 'live', targets=[
            dict(id=t['id'], x_m=t['right_m'], y_m=t['forward_m'],
                 speed_cms=t['spd'], dist_m=t['range_m']) for t in state['targets']])

    @app.after_request
    def no_cache(response):
        response.headers['Cache-Control'] = 'no-store'
        return response

    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8767)
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--radar-port', default='/dev/serial0')
    parser.add_argument('--radar-config', default=str(CONFIG_PATH))
    args = parser.parse_args()
    radar = RadarService(args.radar_port, config_path=args.radar_config)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    radar.start()
    try:
        create_app(radar).run(host=args.host, port=args.port, threaded=True, use_reloader=False)
    finally:
        radar.stop()


if __name__ == '__main__':
    main()
