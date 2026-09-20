"""Shared radar HTTP endpoints for the combined and standalone dashboards."""
from pathlib import Path
from flask import jsonify, request
from radar_trials import RadarTrials


def register_radar_routes(app, radar):
    trials = RadarTrials(radar) if radar else None
    @app.get('/radar')
    def radar_page():
        return (Path(__file__).resolve().parent / 'radar.html').read_text()

    @app.get('/radar/targets')
    def radar_targets():
        if not radar:
            return jsonify(status='disabled', targets=[], raw_targets=[], error='Radar is not enabled'), 503
        return jsonify(radar.snapshot())

    @app.post('/radar/config')
    def radar_config():
        if not radar:
            return jsonify(error='Radar is not enabled'), 503
        try:
            return jsonify(radar.set_config(request.get_json()))
        except ValueError as exc:
            return jsonify(error=str(exc)), 400

    @app.route('/radar/trials', methods=['GET', 'POST'])
    def radar_trials():
        if not trials:
            return jsonify(error='Radar is not enabled'), 503
        if request.method == 'GET':
            return jsonify(trials.snapshot())
        try:
            data = request.get_json()
            if not isinstance(data, dict):
                raise ValueError('Expected a JSON object')
            return jsonify(trials.start(data.get('label'), data.get('right_m'), data.get('forward_m'),
                                        data.get('duration_s', 12))), 202
        except ValueError as exc:
            return jsonify(error=str(exc)), 400
