"""HTTPS editor and upload-only machine inbox. Deliberately has no controller."""
import argparse
import getpass
import io
import json
import secrets
import ssl
from pathlib import Path
from urllib.parse import urlsplit
from flask import Flask, request, jsonify, g, redirect, send_file, send_from_directory, render_template_string, abort
from .web_store import WebStore
from .web_transfer import validate_upload, validate_destination, send_to_receiver
from .cam import generate
from .design_api import register_design_routes
from .profiles import PROFILES

COOKIE='__Host-cnc-session'
LOGIN='__Host-cnc-login'

def create_app(store,public_url,destinations=None,allow_file_upload=False):
    origin=public_url.rstrip('/')
    parsed=urlsplit(origin)
    if parsed.scheme!='https' or not parsed.hostname or parsed.path or parsed.query or parsed.fragment or parsed.username:raise ValueError('Public URL must be an HTTPS origin')
    if not store.has_users():raise ValueError('Create a user first; there are no default credentials')
    destinations=destinations or {}
    for entry in destinations.values():validate_destination(entry)
    app=Flask(__name__,static_folder='static')
    app.config.update(MAX_CONTENT_LENGTH=10*1024*1024,TRUSTED_HOSTS=[parsed.hostname])

    @app.before_request
    def protect():
        if not request.is_secure or request.host!=parsed.netloc:abort(400)
        if request.path=='/api/receive':
            if request.method=='OPTIONS':
                if allow_file_upload and request.headers.get('Origin')=='null':return '',204
                abort(403)
            auth=request.headers.get('Authorization','')
            g.user=store.token_user(auth[7:]) if auth.startswith('Bearer ') else None
            if not g.user:return jsonify(error='Valid upload token required'),401
            return
        if request.path=='/favicon.ico':return '',204
        if request.path=='/login':return
        g.session=store.session(request.cookies.get(COOKIE,''))
        if not g.session:
            if request.path.startswith('/api/'):return jsonify(error='Login expired. Save your project locally, then log in again.'),401
            return redirect('/login')
        g.user=g.session['user']
        if request.method not in ('GET','HEAD','OPTIONS'):
            if request.headers.get('Origin')!=origin or not secrets.compare_digest(request.headers.get('X-Studio-Token',''),g.session['csrf']):abort(403)

    @app.after_request
    def headers(r):
        r.headers.update({'Cache-Control':'no-store','X-Content-Type-Options':'nosniff','Referrer-Policy':'same-origin','Strict-Transport-Security':'max-age=31536000','Content-Security-Policy':"default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; object-src 'none'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'"})
        if request.path=='/api/receive' and allow_file_upload and request.headers.get('Origin')=='null':
            r.headers.update({'Access-Control-Allow-Origin':'null','Access-Control-Allow-Methods':'POST','Access-Control-Allow-Headers':'Authorization, Content-Type','Vary':'Origin'})
        return r

    @app.errorhandler(ValueError)
    def invalid(e):return jsonify(error=str(e)),400

    @app.get('/login')
    def login_form():
        csrf=request.cookies.get(LOGIN) or secrets.token_urlsafe(32)
        response=app.make_response(render_template_string('''<!doctype html><html><meta name="viewport" content="width=device-width"><title>CNC Studio login</title><body><h1>KerfKite Studio</h1><form method="post"><input type="hidden" name="csrf" value="{{ csrf }}"><label>Username <input name="username" autocomplete="username" required maxlength="64"></label><label>Password <input name="password" type="password" autocomplete="current-password" required maxlength="256"></label><button>Log in</button></form></body></html>''',csrf=csrf))
        response.set_cookie(LOGIN,csrf,secure=True,httponly=True,samesite='Strict',max_age=600)
        return response

    @app.post('/login')
    def login():
        expected=request.cookies.get(LOGIN,'')
        if not expected or request.headers.get('Origin')!=origin or not secrets.compare_digest(expected,request.form.get('csrf','')):abort(403)
        session,limited=store.check_login(request.form.get('username'),request.form.get('password'),request.remote_addr)
        if not session:return 'Login failed. Try again later.' if limited else 'Invalid username or password.',429 if limited else 401
        response=redirect('/')
        response.set_cookie(COOKIE,session['sid'],secure=True,httponly=True,samesite='Strict',max_age=8*3600)
        response.delete_cookie(LOGIN,secure=True,httponly=True,samesite='Strict')
        return response

    @app.post('/api/logout')
    def logout():
        store.logout(request.cookies.get(COOKIE,''));r=jsonify(ok=True);r.delete_cookie(COOKIE,secure=True,httponly=True,samesite='Strict');return r

    @app.get('/<path:asset>')
    def asset(asset):
        return send_from_directory(app.static_folder,asset)

    @app.get('/')
    def index():return send_from_directory(app.static_folder,'index.html')
    @app.get('/api/session')
    def session():return jsonify(token=g.session['csrf'],username=g.user,web_mode=True,depth_paths=True,advanced_cam=True, holding_tabs=True)
    @app.get('/api/profiles')
    def profiles():return jsonify(PROFILES)
    @app.post('/api/generate')
    def cam():return jsonify(generate(request.get_json()))
    @app.get('/api/files')
    def files():return jsonify(store.files(g.user))
    @app.post('/api/files')
    def save():
        name,content=validate_upload(request.get_json());return jsonify(store.put_file(g.user,name,content,'web'))
    @app.get('/api/files/<identifier>')
    def download(identifier):
        f=store.file(g.user,identifier)
        if not f:abort(404)
        return send_file(io.BytesIO(f['content']),as_attachment=True,download_name=f['name'],mimetype='application/octet-stream')
    @app.post('/api/files/<identifier>/delete')
    def delete(identifier):
        store.delete_file(g.user,identifier);return jsonify(ok=True)
    @app.route('/api/receive',methods=['POST','OPTIONS'])
    def receive():
        name,content=validate_upload(request.get_json());return jsonify(store.put_file(g.user,name,content,'upload'))
    @app.get('/api/destinations')
    def targets():return jsonify(list(destinations))
    @app.post('/api/send')
    def upload():
        data=request.get_json();name,content=validate_upload(data)
        entry=destinations.get(data.get('destination'))
        if not entry:raise ValueError('Choose an operator-configured receiver')
        return jsonify(send_to_receiver(entry,name,content))
    register_design_routes(app)
    return app


def certificate(cert,key,hosts):
    import datetime
    import ipaddress
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes,serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    if cert.exists() or key.exists():raise ValueError('Certificate/key already exists; refusing to overwrite')
    private=rsa.generate_private_key(public_exponent=65537,key_size=3072)
    names=[]
    for host in hosts:
        try:names.append(x509.IPAddress(ipaddress.ip_address(host)))
        except ValueError:names.append(x509.DNSName(host))
    now=datetime.datetime.now(datetime.timezone.utc)
    subject=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,hosts[0])])
    result=x509.CertificateBuilder().subject_name(subject).issuer_name(subject).public_key(private.public_key()).serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(minutes=5)).not_valid_after(now+datetime.timedelta(days=90)).add_extension(x509.SubjectAlternativeName(names),False).sign(private,hashes.SHA256())
    import os
    with os.fdopen(os.open(key,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),'wb') as f:f.write(private.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    with cert.open('xb') as f:f.write(result.public_bytes(serialization.Encoding.PEM))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--data-dir',default='web-data');sub=p.add_subparsers(dest='action',required=True)
    for action in ('create-user','reset-password'):
        q=sub.add_parser(action);q.add_argument('username');q.add_argument('--generate-password',action='store_true')
    for action in ('issue-token','revoke-tokens'):
        q=sub.add_parser(action);q.add_argument('username')
    q=sub.add_parser('certificate');q.add_argument('--cert',required=True);q.add_argument('--key',required=True);q.add_argument('--hosts',nargs='+',required=True)
    q=sub.add_parser('destination');q.add_argument('name');q.add_argument('--url',required=True);q.add_argument('--ca-file');q.add_argument('--allow-http',action='store_true')
    q=sub.add_parser('serve');q.add_argument('--public-url',required=True);q.add_argument('--cert',required=True);q.add_argument('--key',required=True);q.add_argument('--host',default='0.0.0.0');q.add_argument('--port',type=int,default=8443);q.add_argument('--allow-file-upload',action='store_true')
    a=p.parse_args()
    try:
        if a.action=='certificate':certificate(Path(a.cert),Path(a.key),a.hosts);return
        store=WebStore(a.data_dir)
        if a.action in ('create-user','reset-password'):
            password=secrets.token_urlsafe(32) if a.generate_password else getpass.getpass('Password (15–256 characters): ')
            if not a.generate_password and password!=getpass.getpass('Repeat password: '):raise ValueError('Passwords did not match')
            store.add_user(a.username,password,replace=a.action=='reset-password')
            print('Account configured.')
            if a.generate_password:print('Save this generated password now: '+password)
        elif a.action=='issue-token':print(store.issue_token(a.username))
        elif a.action=='revoke-tokens':store.revoke_tokens(a.username)
        elif a.action=='destination':
            path=store.root/'destinations.json';entries=json.loads(path.read_text()) if path.exists() else {}
            entries[a.name]=validate_destination({'url':a.url,'ca_file':str(Path(a.ca_file).resolve()) if a.ca_file else None,'allow_http':a.allow_http,'token':getpass.getpass('Receiver upload token: ')})
            import os
            with os.fdopen(os.open(path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w') as f:json.dump(entries,f)
        elif a.action=='serve':
            from cheroot.wsgi import Server
            from cheroot.ssl.builtin import BuiltinSSLAdapter
            path=store.root/'destinations.json';entries=json.loads(path.read_text()) if path.exists() else {}
            app=create_app(store,a.public_url,entries,a.allow_file_upload)
            server=Server((a.host,a.port),app,numthreads=8)
            server.ssl_adapter=BuiltinSSLAdapter(a.cert,a.key)
            server.ssl_adapter.context.minimum_version=ssl.TLSVersion.TLSv1_2
            print('HTTPS editor and upload inbox: '+a.public_url,flush=True)
            try:server.start()
            finally:server.stop()
    except (ValueError,OSError) as e:p.error(str(e))

if __name__=='__main__':main()
