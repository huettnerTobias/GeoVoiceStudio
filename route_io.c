
/* Unmarked Java libraries used quote-only escaping. Preserve their literal backslashes. */
static char *legacy_strings(const char *src,size_t n,size_t *out_len) {
    char *out=malloc(n*6+1);
    if (!out) { memory_error(); return NULL; }
    size_t j=0; int quoted=0;
    static const char hex[]="0123456789abcdef";
    for (size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)src[i];
        if (quoted && c=='\\') {
            out[j++]='\\';
            if (i+1<n && src[i+1]=='"') { out[j++]='"'; ++i; }
            else out[j++]='\\';
        } else if (quoted && c<32) {
            memcpy(out+j,"\\u00",4); j+=4; out[j++]=hex[c>>4]; out[j++]=hex[c&15];
        } else {
            if (c=='"') quoted=!quoted;
            out[j++]=(char)c;
        }
    }
    out[j]=0; *out_len=j; return out;
}
static int java_routes(const wchar_t *dir,const wchar_t *suffix,GvLibrary *imports) {
    wchar_t path[GV_AUDIO_PATH],marker[GV_AUDIO_PATH],basename[64];
    swprintf(basename,N(basename),L"routes.json%ls",suffix);
    if (!join(path,dir,basename)) return 0;
    swprintf(basename,N(basename),L"routes.json.format.json%ls",suffix);
    if (!join(marker,dir,basename)) return 0;
    int standard=GetFileAttributesW(marker)!=INVALID_FILE_ATTRIBUTES;
    if (standard) {
        size_t n; char *json=read_small(marker,&n);
        yyjson_doc *doc=json?parse_json(json,n):NULL;
        free(json);
        yyjson_val *root=doc?yyjson_doc_get_root(doc):NULL,*v=yyjson_obj_get(root,"formatVersion");
        int ok=yyjson_is_uint(v) && yyjson_get_uint(v)==1 && yyjson_equals_str(yyjson_obj_get(root,"stringEncoding"),"standard-json");
        yyjson_doc_free(doc);
        if (!ok) return fail(L"Ungültiger Formatmarker der Java-Bibliothek.");
    }
    size_t n=0; char *s=read_small(path,&n);
    if (!s) return 0;
    if (!standard) {
        char *canonical=legacy_strings(s,n,&n); free(s); s=canonical;
        if (!s) return 0;
    }
    yyjson_doc *doc=parse_json(s,n);
    free(s);
    int ok=doc && parse_routes(yyjson_doc_get_root(doc),imports,0);
    yyjson_doc_free(doc);
    return ok;
}
static int copy_stage(const GvLibrary *lib,const wchar_t *src,Media *m) {
    if (!regular(src)) return fail(L"Audiodatei fehlt oder ist nicht lesbar: %ls",src);
    if (!make_stage(lib,m)) return 0;
    if (!CopyFileW(src,m->stage,TRUE)) return fail(L"Audio konnte nicht kopiert werden: %ls",src);
    SetFileAttributesW(m->stage,FILE_ATTRIBUTE_NORMAL);
    return hash_path(m->stage,m->sha,&m->size);
}
static int read_java(const GvLibrary *lib,const wchar_t *dir,GvLibrary *incoming,MediaList *media) {
    wchar_t path[GV_AUDIO_PATH];
    if (!join(path,dir,L".geovoice.lock")) return 0;
    HANDLE lock=INVALID_HANDLE_VALUE; OVERLAPPED ov={0};
    if (regular(path)) {
        lock=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if (lock==INVALID_HANDLE_VALUE || !LockFileEx(lock,LOCKFILE_FAIL_IMMEDIATELY,0,MAXDWORD,0x7fffffff,&ov)) {
            if (lock!=INVALID_HANDLE_VALUE) CloseHandle(lock);
            return fail(L"Bitte die Java-Anwendung vor dem Import schließen.");
        }
    }
    int ok=java_routes(dir,L"",incoming);
    if (!ok && join(path,dir,L"routes.json.bak") && regular(path)) {
        gv_free(incoming);
        ok=java_routes(dir,L".bak",incoming);
    }
    if (ok) ok=collect_media(incoming->routes,incoming->count,media);
    wchar_t audio[GV_AUDIO_PATH]; uint64_t total=0;
    if (ok) ok=join(audio,dir,L"audio");
    if (ok && media->count) {
        DWORD attr=GetFileAttributesW(audio);
        if (attr==INVALID_FILE_ATTRIBUTES || !(attr&FILE_ATTRIBUTE_DIRECTORY) || (attr&FILE_ATTRIBUTE_REPARSE_POINT))
            ok=fail(L"Der Audioordner fehlt oder verweist außerhalb der Bibliothek.");
    }
    for (size_t i=0;ok && i<media->count;i++) {
        Media *m=media->files+i;
        ok=join(path,audio,m->name) && copy_stage(lib,path,m);
        if (ok && (total+=m->size)>TOTAL_MAX) ok=fail(L"Bibliothek überschreitet 8 GiB Audio.");
    }
    if (lock!=INVALID_HANDLE_VALUE) CloseHandle(lock);
    return ok;
}
static int same_digest(const wchar_t *path,const Media *m,int *same) {
    char sha[65]; uint64_t n;
    if (!hash_path(path,sha,&n)) return 0;
    *same=n==m->size && !strcmp(sha,m->sha); return 1;
}
static int install_media(const GvLibrary *lib,Media *m) {
    const wchar_t *ext=wcsrchr(m->name,L'.');
    wchar_t target[GV_AUDIO_PATH],candidate[256];
    if (!copy_text(candidate,N(candidate),m->name)) return 0;
    for (unsigned attempt=0;attempt<10000;attempt++) {
        if (!gv_audio_path(lib,candidate,target)) return 0;
        WIN32_FIND_DATAW info;
        HANDLE found=FindFirstFileW(target,&info);
        if (found==INVALID_HANDLE_VALUE) {
            if (GetLastError()!=ERROR_FILE_NOT_FOUND) return fail(L"Audioziel ist nicht zugänglich: %ls",target);
            if (MoveFileExW(m->stage,target,MOVEFILE_WRITE_THROUGH)) {
                wcscpy(m->installed,candidate); *m->stage=0; m->created=1; return 1;
            }
            if (GetLastError()!=ERROR_ALREADY_EXISTS && GetLastError()!=ERROR_FILE_EXISTS)
                return fail(L"Audiodatei kann nicht übernommen werden: %ls",candidate);
        } else {
            FindClose(found);
            int same=0;
            if (!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {
                if (!same_digest(target,m,&same)) return 0;
                if (same) {
                    if (!audio_name(info.cFileName)) return 0;
                    return copy_text(m->installed,N(m->installed),info.cFileName);
                }
            }
        }
        /* Shorten long names by UTF-8 length before adding the collision suffix. */
        size_t chars=(size_t)(ext-m->name);
        for (;;) {
            swprintf(candidate,N(candidate),L"%.*ls-%.10hs-%u%ls",(int)chars,m->name,m->sha,attempt+1,ext);
            if (audio_name(candidate)) break;
            if (!chars) return fail(L"Es konnte kein freier Audioname bestimmt werden.");
            --chars;
        }
    }
    return fail(L"Zu viele gleichnamige Audiodateien.");
}
static void cleanup_media(const GvLibrary *lib,MediaList *media,int undo) {
    for (size_t i=0;i<media->count;i++) {
        Media *m=media->files+i; wchar_t path[GV_AUDIO_PATH];
        if (*m->stage) DeleteFileW(m->stage);
        if (undo && m->created && gv_audio_path(lib,m->installed,path)) DeleteFileW(path);
    }
    free(media->files);
}
int gv_import_audio(const GvLibrary *lib,const wchar_t *src,wchar_t outName[256]) {
    Media m={0};
    const wchar_t *name=src;
    for (const wchar_t *p=src;*p;p++) if (*p=='\\' || *p=='/') name=p+1;
    if (!audio_name(name) || !copy_text(m.name,N(m.name),name)) return 0;
    int ok=copy_stage(lib,src,&m) && install_media(lib,&m);
    if (*m.stage) DeleteFileW(m.stage);
    if (ok) wcscpy(outName,m.installed);
    return ok;
}
static size_t find_route(const GvLibrary *lib,const wchar_t *id) {
    for (size_t i=0;i<lib->count;i++) if (!wcscmp(lib->routes[i].id,id)) return i;
    return lib->count;
}
int gv_import(GvLibrary *lib,const wchar_t *source,int replace) {
    GvLibrary incoming={0},merged={0}; MediaList media={0};
    DWORD attr=GetFileAttributesW(source);
    if (attr==INVALID_FILE_ATTRIBUTES || (attr&FILE_ATTRIBUTE_REPARSE_POINT)) return fail(L"Die gewählte Paketdatei oder Bibliothek ist nicht verfügbar.");
    int ok=(attr&FILE_ATTRIBUTE_DIRECTORY)?read_java(lib,source,&incoming,&media):read_package(lib,source,&incoming,&media);
    if (ok && !replace) {
        size_t overlaps=0;
        for (size_t i=0;i<incoming.count;i++) if (find_route(lib,incoming.routes[i].id)<lib->count) overlaps++;
        if (overlaps) {
            fail(L"%zu vorhandene Route(n) werden ersetzt. Fortfahren?",overlaps); ok=-1;
        }
    }
    if (ok==1) {
        memcpy(merged.dir,lib->dir,sizeof(merged.dir));
        merged.count=lib->count;
        size_t cap=merged.count+incoming.count;
        merged.routes=calloc(cap?cap:1,sizeof(*merged.routes));
        if (!merged.routes) ok=memory_error();
        else if (merged.count) memcpy(merged.routes,lib->routes,merged.count*sizeof(*merged.routes));
    }
    for (size_t i=0;ok==1 && i<media.count;i++) ok=install_media(lib,media.files+i);
    if (ok==1) {
        for (size_t i=0;i<incoming.count;i++) {
            GvRoute *r=incoming.routes+i;
            for (size_t j=0;j<r->count;j++) {
                GvPoint *p=r->points+j;
                if (*p->audio) wcscpy(p->audio,lookup(&media,p->audio)->installed);
            }
            size_t index=find_route(lib,r->id);
            if (index==lib->count) index=merged.count++;
            merged.routes[index]=*r;
        }
        /* The new and old arrays share point buffers until the replacement succeeds. */
        ok=gv_save(&merged);
        if (ok) {
            for (size_t i=0;i<lib->count;i++) if (lib->routes[i].points!=merged.routes[i].points) free(lib->routes[i].points);
            free(lib->routes); *lib=merged; merged.routes=NULL;
            for (size_t i=0;i<incoming.count;i++) incoming.routes[i].points=NULL;
        }
    }
    free(merged.routes); gv_free(&incoming);
    cleanup_media(lib,&media,ok!=1);
    return ok;
}
static int references(const GvLibrary *lib,const wchar_t *name) {
    for (size_t i=0;i<lib->count;i++) for (size_t j=0;j<lib->routes[i].count;j++)
        if (!name_compare(lib->routes[i].points[j].audio,name)) return 1;
    return 0;
}
int gv_delete_audio(GvLibrary *lib,const wchar_t *name) {
    wchar_t path[GV_AUDIO_PATH];
    if (!gv_audio_path(lib,name,path)) return 0;
    if (references(lib,name)) return fail(L"„%ls“ wird in einer Route verwendet. Bitte zuerst die Audiozuordnung ändern.",name);
    return (regular(path) && DeleteFileW(path)) || fail(L"Audiodatei kann nicht gelöscht werden: %ls",name);
}
int gv_rename_audio(GvLibrary *lib,const wchar_t *oldName,const wchar_t *newName) {
    wchar_t src[GV_AUDIO_PATH],dest[GV_AUDIO_PATH];
    if (!gv_audio_path(lib,oldName,src) || !gv_audio_path(lib,newName,dest)) return 0;
    if (_wcsicmp(wcsrchr(oldName,L'.'),wcsrchr(newName,L'.'))) return fail(L"Die Dateiendung muss beibehalten werden.");
    if (!regular(src)) return fail(L"Die Audiodatei ist nicht verfügbar.");
    if (!wcscmp(oldName,newName)) return 1;
    int case_only=!name_compare(oldName,newName);
    if (!case_only && (references(lib,newName) || !CopyFileW(src,dest,TRUE))) return fail(L"Der neue Dateiname ist bereits vergeben oder nicht verfügbar.");
    if (case_only && !MoveFileExW(src,dest,MOVEFILE_WRITE_THROUGH)) return fail(L"Umbenennen fehlgeschlagen.");
    size_t n=0;
    for (size_t i=0;i<lib->count;i++) for (size_t j=0;j<lib->routes[i].count;j++) {
        GvPoint *p=lib->routes[i].points+j;
        if (!name_compare(p->audio,oldName)) { wcscpy(p->audio,newName); ++n; }
    }
    if (n && !gv_save(lib)) {
        for (size_t i=0;i<lib->count;i++) for (size_t j=0;j<lib->routes[i].count;j++) {
            GvPoint *p=lib->routes[i].points+j;
            if (!wcscmp(p->audio,newName)) wcscpy(p->audio,oldName);
        }
        if (case_only) MoveFileW(dest,src); else DeleteFileW(dest);
        return 0;
    }
    if (!case_only && !DeleteFileW(src)) return fail(L"Zuordnungen wurden aktualisiert. Die alte Datei ist noch in der Audioliste.");
    return 1;
}

