"""Exercise the native hosts' document commands against a running simulation in a browser."""
import json


def run(page):
    page.wait_for_function("window.KerfKiteDesktop && JSON.parse(KerfKiteDesktop.dispatch('state')).ok")

    def call(action, payload=None, *, success=True):
        result = page.evaluate("""async ([action,payload]) => {
          const run=(a,p)=>JSON.parse(window.KerfKiteDesktop.dispatch(a,p));
          let reply=run(action,payload);
          const deadline=Date.now()+30000;
          while(reply.pending) {
            if(Date.now()>deadline) throw Error('Timed out waiting for '+action);
            await new Promise(resolve=>setTimeout(resolve,25));
            reply=run('result',{id:reply.pending});
          }
          return reply;
        }""", [action, payload or {}])
        assert result["ok"] == success, (action, result)
        return result.get("value", result.get("error"))

    def insert(kind):
        return call("shape", {"kind":kind,"x":5,"y":7,"width":12,"height":9})

    initial = call("state")
    assert not initial["dirty"] and not initial["objects"]
    assert insert("rectangle")["dirty"]
    call("move", {"x":17, "y":19})
    assert call("state")["selection"]["x"] == 17
    call("preview")
    assert "G1" in call("export")["content"]
    snapshot = call("save")
    insert("ellipse")
    assert call("saved", snapshot)["dirty"]  # An edit landed while a file dialog was open.
    call("export", success=False)
    call("undo")
    assert len(call("state")["objects"]) == 1
    call("redo")
    insert("line")
    call("select", {"index":0})
    call("move", {"x":-1,"y":0}, success=False)
    assert len(call("state")["objects"]) == 3
    for name in ("3D CAD","Projects","CAM","Templates","Layers","Control","Text","Images","Properties","Help","Design"):
        call("tab", {"name":name})
        assert page.get_by_role("tab", name=name, exact=True).get_attribute("aria-selected") == "true"
    doc = json.loads(snapshot["content"])
    doc["metadata"]["name"] = "Native résumé"
    doc["extras"]["desktopFixture"] = {"stock":"birch"}
    doc["objects"][0]["rotationPivot"] = {"u":0.2, "v":0.7}
    opened = call("open", {"content":json.dumps(doc)})
    assert not opened["dirty"] and opened["title"] == "Native résumé"
    assert opened["documentId"] != snapshot["documentId"]
    new_snapshot = call("save")
    restored = json.loads(new_snapshot["content"])
    assert restored["extras"]["desktopFixture"] == doc["extras"]["desktopFixture"]
    assert restored["objects"] == doc["objects"]
    call("saved", snapshot, success=False)
    for content in ("{", json.dumps({**doc,"objects":[{"paths":[[[1,1]]],"name":"bad"}]})):
        call("open", {"content":content}, success=False)
        assert call("save") == new_snapshot
    call("new")
    call("undo")
    assert not call("state")["objects"]
    print("PASS native commands, tab navigation, edits, undo, portable project round trip, failed open and current-preview export")
