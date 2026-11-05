#!/usr/bin/env python3
"""Exercise real ZIP verification, HTTPS, installation, service locks and recovery."""
from pathlib import Path
import hashlib,http.server,os,socketserver,ssl,struct,subprocess,sys,tempfile,threading,time,unittest,zipfile
BINARY=Path(sys.argv.pop(1)).resolve()
CONTROLLER=BINARY.with_name('easy-connect')
UI_BINARY=BINARY.with_name('ec-ui-test')
PROJECT=Path(__file__).resolve().parents[1]
FAMILY='pocketbook-armv7-softfp'
MODELS='632 610\n634 610\n700 610\n743G 610\n'
NAMES=['applications/EasyConnect.app','system/share/easy-connect/easy-connect.conf.default']+['system/share/easy-connect/bin/'+n for n in ('easy-connect','easy-connect.app','dropbear','dropbearkey','vsftpd')]
def elf():
    data=bytearray(128);data[:7]=b'\x7fELF\x01\x01\x01';struct.pack_into('<HH',data,16,2,40);struct.pack_into('<I',data,36,0x5000200);return bytes(data)
class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='ec-update-');self.addCleanup(self.temp.cleanup)
        self.directory=Path(self.temp.name);self.storage=self.directory/'storage'
        self.root=self.storage/'system/share/easy-connect';self.root.mkdir(parents=True)
        self.zip=self.directory/'update.zip';self.channel=self.directory/'channel.txt'
        self.package()
    def package(self,*,version='0.3.1',family=FAMILY,models=MODELS,entries=None,compression=zipfile.ZIP_DEFLATED):
        entries=entries if entries is not None else [(n,(PROJECT/'data/EasyConnect.app').read_bytes() if n.endswith('/EasyConnect.app') else b'auth=password\n' if n.endswith('.default') else elf()) for n in NAMES]
        with zipfile.ZipFile(self.zip,'w',compression) as archive:
            for name,data in entries:archive.writestr(name,data)
            archive.comment=('EC-PACKAGE-1\n'+family+'\n'+version+'\n'+models).encode()
        self.channel.write_text('EC-OTA-SHARED-1\n'+FAMILY+'\n'+version+'\n'+hashlib.sha256(self.zip.read_bytes()).hexdigest()+'\n'+MODELS)
    def run_driver(self,command,*,expected=0,model='PB632',firmware='U632.6.11.9999',extra=None):
        argv=[str(BINARY),command,str(self.root),str(self.channel),model,firmware]
        if command!='parse':argv.append(str(extra or self.zip))
        r=subprocess.run(argv,capture_output=True,text=True,timeout=20)
        self.assertEqual(r.returncode,expected,r.stderr);return r
    def run_controller(self,command,expected=0):
        r=subprocess.run([str(CONTROLLER),'--root',str(self.root),command],capture_output=True,text=True,timeout=20)
        self.assertEqual(r.returncode,expected,r.stderr);return r
    def stage(self):self.run_driver('stage');self.run_driver('request')
    def old_install(self):
        (self.root/'bin').mkdir();(self.root/'bin/easy-connect').symlink_to(CONTROLLER)
        (self.root/'bin/easy-connect.app').write_text('#!/bin/sh\nexit 0\n');(self.root/'bin/easy-connect.app').chmod(0o755)
        (self.root/'easy-connect.conf').write_text('auth=both\nssh_port=2244\n')
        (self.root/'state').mkdir();(self.root/'state/password.hash').write_text('private verifier')
        (self.root/'state/host_ed25519').write_text('private host key')
        (self.root/'ui.log').write_text('diagnostic evidence')
    def test_semver_numeric_and_invalid_versions(self):
        for a,b,expected in [('0.3.10','0.3.2',0),('1.0.0','0.99.99',0),('0.3.0','0.3.0',1),('0.2.9','0.3.0',1),('01.0.0','0.3.0',1),('1.2','0.3.0',1),('1.2.3.4','0.3.0',1),('1.2.3-beta','0.3.0',1),('9999999.0.0','0.3.0',1)]:
            self.assertEqual(subprocess.run([str(BINARY),'newer',a,b]).returncode,expected,(a,b))
    def test_compatible_models_and_newer_firmware_patches(self):
        for model,firmware in [('PB632','U632.6.11.9999'),('PocketBook 634','U634.6.10.9999'),('PB700','U700.6.12.1'),('PB743g','U743g.6.11.9999')]:self.run_driver('parse',model=model,firmware=firmware)
        self.run_driver('parse',model='PB1030',expected=1);self.run_driver('parse',firmware='U632.6.9.1',expected=1)
    def test_reject_malformed_channels_and_duplicate_models(self):
        original=self.channel.read_text()
        for invalid in [original.replace('EC-OTA','CR3-OTA'),original.replace(FAMILY,'other-family'),original.replace('0.3.1','00.3.1'),original+'632 610\n',original+'634',original.replace('632 610','632 6110'),original.replace('632 610','632 +610'),original.replace('632 610','632 609'),original+'x'*17000,original+'\0hidden']:
            self.channel.write_text(invalid);self.run_driver('parse',expected=1)
    def test_normalized_update_root_keeps_staging_outside_the_installation(self):
        result=subprocess.run([str(BINARY),'stage',str(self.root)+'/',str(self.channel),'PB632','U632.6.11.1',str(self.zip)],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((self.root/'-ota-new').exists())
        self.assertTrue(Path(str(self.root)+'-ota-new/.ready').exists())

    def test_stored_and_deflated_archives(self):
        for compression in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED):self.package(compression=compression);self.run_driver('archive')
    def test_checksum_damage_is_rejected(self):
        self.zip.write_bytes(self.zip.read_bytes()+b'damage');self.run_driver('archive',expected=1)
    def test_exact_allowlist_blocks_traversal_extras_and_missing_files(self):
        entries=[(n,b'#!/bin/sh\nexit 0\n' if n.endswith('/EasyConnect.app') else b'auth=password\n' if n.endswith('.default') else elf()) for n in NAMES]
        for changed in [entries[:-1],entries+[('README.txt',b'not runtime')],entries+[('../outside',b'escape')],entries[:-1]+[('/absolute',elf())],entries[:-1]+[(NAMES[0],b'duplicate')]]:
            self.package(entries=changed);self.run_driver('archive',expected=1)
        self.assertFalse((self.directory/'outside').exists())
    def test_package_metadata_family_version_and_api_must_match(self):
        for kwargs in ({'family':'other'},{'models':'632 6110\n'},{'version':'0.3.2'}):
            self.package(**kwargs)
            if 'version' in kwargs:self.channel.write_text(self.channel.read_text().replace('0.3.2','0.3.1'))
            self.run_driver('archive',expected=1)
    def test_symlinks_foreign_architecture_and_crc_are_rejected(self):
        entries=[(n,b'#!/bin/sh\nexit 0\n' if n.endswith('/EasyConnect.app') else b'auth=password\n' if n.endswith('.default') else elf()) for n in NAMES]
        info=zipfile.ZipInfo(NAMES[-1]);info.create_system=3;info.external_attr=(0o120777<<16)
        self.package(entries=entries[:-1]+[(info,b'/tmp/unsafe')]);self.run_driver('archive',expected=1)
        wrong=bytearray(elf());struct.pack_into('<H',wrong,18,62)
        self.package(entries=entries[:-1]+[(NAMES[-1],bytes(wrong))]);self.run_driver('archive',expected=1)
        hard=bytearray(elf());struct.pack_into('<I',hard,36,0x5000400)
        self.package(entries=entries[:-1]+[(NAMES[-1],bytes(hard))]);self.run_driver('archive',expected=1)
        self.package(compression=zipfile.ZIP_STORED);data=bytearray(self.zip.read_bytes());data[data.index(b'auth=password')]=ord('x');self.zip.write_bytes(data)
        lines=self.channel.read_text().splitlines();lines[3]=hashlib.sha256(data).hexdigest();self.channel.write_text('\n'.join(lines)+'\n');self.run_driver('archive',expected=1)
    def test_stage_requires_newer_version_and_does_not_change_install(self):
        self.old_install();before=(self.root/'easy-connect.conf').read_bytes();self.stage()
        self.assertEqual((self.root/'easy-connect.conf').read_bytes(),before)
        ready=Path(str(self.root)+'-ota-new/.ready').read_bytes()
        self.package(version='0.2.0');self.run_driver('stage',expected=1)
        self.assertEqual(Path(str(self.root)+'-ota-new/.ready').read_bytes(),ready)
    def test_install_reverifies_staged_zip_and_preserves_every_user_file(self):
        self.old_install();self.stage();self.run_controller('update-install')
        for name,value in [('easy-connect.conf','auth=both\nssh_port=2244\n'),('state/password.hash','private verifier'),('state/host_ed25519','private host key'),('ui.log','diagnostic evidence')]:self.assertEqual((self.root/name).read_text(),value)
        self.assertEqual((self.root/'bin/easy-connect.app').read_bytes(),elf())
        self.assertFalse((self.root/'EasyConnect.app').exists());self.assertTrue((self.root/'.ota-first-run').exists())
        self.assertTrue(Path(str(self.root)+'-ota-old/bin/easy-connect.app').exists())
        self.assertFalse(Path(str(self.root)+'-ota-new').exists())
    def test_rollback_preserves_state_written_during_the_first_run(self):
        self.old_install();self.stage();self.run_controller('update-install')
        (self.root/'state/host_ed25519').write_text('new host key retained')
        (self.root/'easy-connect.conf').write_text('auth=password\nssh_port=2255\n')
        self.run_controller('update-rollback')
        self.assertEqual((self.root/'state/host_ed25519').read_text(),'new host key retained')
        self.assertIn('2255',(self.root/'easy-connect.conf').read_text())
        self.assertFalse((self.root/'.ota-first-run').exists())
        self.assertFalse(Path(str(self.root)+'-ota-new').exists())

    def test_tampered_stage_never_switches_installation(self):
        self.old_install();self.stage();Path(str(self.root)+'-ota-new/package.zip').write_bytes(b'changed')
        self.run_controller('update-install',expected=1);self.assertFalse((self.root/'.ota-first-run').exists())
    def test_active_service_lock_prevents_installation(self):
        import fcntl
        self.old_install();self.stage();directory=Path('/tmp')/f'easy-connect-{os.geteuid()}';directory.mkdir(exist_ok=True,mode=0o700)
        with (directory/'lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            result=self.run_controller('update-install',expected=1)
        self.assertIn('Stop both SSH and FTP',result.stderr);self.assertFalse((self.root/'.ota-first-run').exists())
    def test_production_launcher_rolls_back_unbootable_update_using_old_helper(self):
        self.old_install();self.stage()
        # An ARM payload cannot boot on the host. This exercises real rollback after failed exec.
        flag=self.root/'old-ui-ran'
        script=f'#!/bin/sh\nif [ ! -f "{flag}" ]; then touch "{flag}"; exit 75; fi\nexit 0\n'
        (self.root/'bin/easy-connect.app').write_text(script)
        launcher=self.directory/'launcher';launcher.write_text((PROJECT/'data/EasyConnect.app').read_text().replace('/mnt/ext1',str(self.storage)))
        result=subprocess.run(['sh',str(launcher)],capture_output=True,text=True,timeout=25)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual((self.root/'bin/easy-connect.app').read_text(),script)
        self.assertTrue(Path(str(self.root)+'-ota-failed/.ota-first-run').exists())
        self.assertEqual((self.root/'state/host_ed25519').read_text(),'private host key')
    def test_launcher_recovers_interrupted_directory_switch(self):
        self.old_install();self.root.rename(str(self.root)+'-ota-old')
        launcher=self.directory/'launcher';launcher.write_text((PROJECT/'data/EasyConnect.app').read_text().replace('/mnt/ext1',str(self.storage)))
        result=subprocess.run(['sh',str(launcher)],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr);self.assertTrue((self.root/'bin/easy-connect.app').exists())
    def test_launcher_finishes_interrupted_rollback_without_retrying_bad_update(self):
        self.old_install()
        rollback=Path(str(self.root)+'-ota-rollback');self.root.rename(rollback)
        (rollback/'.ota-rollback-ready').write_text('ready\n')
        stage=Path(str(self.root)+'-ota-new');stage.mkdir();(stage/'.ready').write_text('bad release')
        launcher=self.directory/'launcher';launcher.write_text((PROJECT/'data/EasyConnect.app').read_text().replace('/mnt/ext1',str(self.storage)))
        result=subprocess.run(['sh',str(launcher)],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((self.root/'.ota-rollback-ready').exists());self.assertFalse((stage/'.ready').exists())

    def test_https_requires_trusted_certificate_and_limits_downloads(self):
        cert=self.directory/'cert.pem';key=self.directory/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        fixture=self
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path=='/redirect':self.send_response(302);self.send_header('Location','http://localhost/insecure');self.end_headers();return
                if self.path.endswith('.zip'): data=fixture.zip.read_bytes()
                elif self.path.startswith('/ota/'): data=fixture.channel.read_bytes()
                else: data=b'x'*2048 if self.path=='/large' else b'EC test channel\n'
                self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers()
                try:self.wfile.write(data)
                except (BrokenPipeError,ssl.SSLError):pass
            def log_message(self,*args):pass
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key);server.socket=context.wrap_socket(server.socket,server_side=True)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();self.addCleanup(server.server_close);self.addCleanup(server.shutdown)
        url=f'https://localhost:{server.server_port}'
        def fetch(path,expected,trust=True,output=None,limit=None):
            env={**os.environ,'NO_PROXY':'localhost,127.0.0.1','SSL_CERT_FILE':str(cert if trust else self.directory/'missing.pem')}
            args=[str(BINARY),'fetch',url+path]
            if output:args+=[str(output)]
            if limit:args+=[str(limit)]
            result=subprocess.run(args,capture_output=True,text=True,env=env,timeout=15);self.assertEqual(result.returncode,expected,result.stderr);return result
        self.assertIn('EC test channel',fetch('/',0).stdout);fetch('/',1,trust=False)
        output=self.directory/'download';fetch('/large',1,output=output,limit=1024);self.assertFalse(output.exists())
        fetch('/redirect',1)
        events=['press check-updates']+['wait 10']*100+['dialog 1']+['wait 10']*150+['dialog 2','press version','dialog 1']
        environment={**os.environ,'NO_PROXY':'localhost,127.0.0.1','SSL_CERT_FILE':str(cert),
                     'EC_UI_ROOT':str(self.root),'EC_UI_EVENTS':'-','EC_UI_ADDRESS':'192.0.2.1',
                     'EC_UI_UPDATE_ORIGIN':url+'/', 'EC_UI_SCANNER':str(self.directory/'missing-scanner')}
        ui=UI_BINARY
        result=subprocess.run([str(ui)],input='\n'.join(events)+'\n',capture_output=True,text=True,env=environment,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('Available: 0.3.1',result.stdout)
        self.assertIn('is verified and ready',result.stdout)
        self.assertIn('Version: Easy Connect 0.3.0',result.stdout)
        self.assertTrue(Path(str(self.root)+'-ota-new/.ready').exists())
        self.assertFalse((self.root/'.ota-first-run').exists())
        # Later defers installation; a subsequent idle launch hands off to the bootstrap.
        result=subprocess.run([str(ui)],input='',capture_output=True,text=True,env=environment,timeout=10)
        self.assertEqual(result.returncode,75,result.stderr)
        self.assertTrue(Path(str(self.root)+'-ota-new/.install').exists())
        self.assertEqual(subprocess.run([str(BINARY),'fetch','http://localhost/']).returncode,1)
if __name__=='__main__':unittest.main()
