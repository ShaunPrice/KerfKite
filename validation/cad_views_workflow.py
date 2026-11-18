"""Exercise CAD cameras with actual viewport keyboard and pointer input."""
import math

from playwright.sync_api import expect


def run(page):
    loc = page.locator
    page.evaluate("() => window.studioReady")
    page.wait_for_function("window.KerfKiteViews")
    canvas = loc("#canvas")
    screen = lambda x,y: canvas.evaluate(
        "(el,[x,y])=>{const p=new DOMPoint(x,Number(document.getElementById('height').value)-y)"
        ".matrixTransform(el.getScreenCTM());return [p.x,p.y];}", [x,y])
    snapshot = lambda mode: page.evaluate("mode=>KerfKiteViews.state(mode)",mode)
    call = lambda name,args=None: page.evaluate("([n,a])=>KerfKiteViews.apply(n,a)",[name,args or []])

    page.get_by_role("tab",name="Design",exact=True).click()
    for key,value in (("shapeX","30"),("shapeY","30"),("shapeW","40"),("shapeH","20")):
        loc("#"+key).fill(value)
    loc("#rect").click()
    canvas.focus()
    bed = snapshot("2d")
    page.keyboard.press("f")
    fitted = snapshot("2d")
    assert fitted["viewBox"][2] < bed["viewBox"][2]
    anchor = [round(v) for v in screen(44,41)]
    point = canvas.evaluate("(el,[x,y])=>{const p=new DOMPoint(x,y).matrixTransform(el.getScreenCTM().inverse());"
                            "return [p.x,Number(document.getElementById('height').value)-p.y];}",anchor)
    page.mouse.move(*anchor)
    page.mouse.wheel(0,-150)
    page.wait_for_function("w=>KerfKiteViews.state('2d').viewBox[2]<w",arg=fitted["viewBox"][2])
    assert math.dist(anchor,screen(*point)) < 0.1
    page.mouse.down(button="middle")
    page.mouse.move(anchor[0]+16,anchor[1]+12,steps=3)
    page.mouse.up(button="middle")
    assert math.dist(screen(*point),[anchor[0]+16,anchor[1]+12]) < 0.1
    panned = snapshot("2d")
    start = screen(35,36)
    end = screen(39,39)
    page.mouse.move(*start)
    page.mouse.down()
    page.mouse.move(*end,steps=4)
    page.mouse.up()
    extent = page.evaluate("()=>CNCLayout.extent(CNCProjects.document().objects[0])")
    assert abs(extent[0]-34)<.2 and abs(extent[1]-33)<.2, extent
    assert snapshot("2d")==panned
    page.get_by_role("tab",name="Design",exact=True).click()
    loc("#shapeX").focus()
    expect(loc("#shapeX")).to_be_focused()
    page.keyboard.press("Numpad3")
    assert page.evaluate("KerfKiteViews.current()")=="2d"
    canvas.focus()
    page.keyboard.press("Numpad3")
    assert snapshot("3d")["workspace"]=="3d" and abs(snapshot("3d")["yaw"]+math.pi/2)<1e-9
    loc("#solid-add").click()
    expect(loc("#solid-status")).to_have_text("Solid ready.")
    model=page.evaluate("CNCSolidExchange.model()")
    solid=loc("#solid-canvas")
    solid.focus()
    page.keyboard.press("Numpad7")
    assert abs(snapshot("3d")["pitch"]-math.pi/2)<1e-9
    page.keyboard.press("ArrowRight")
    page.keyboard.press("Shift+ArrowDown")
    page.keyboard.press("+")
    before=snapshot("3d")
    assert before["yaw"]>0 and before["panY"]>0 and before["zoom"]>1
    page.keyboard.press("f")
    after=snapshot("3d")
    assert after["yaw"]==before["yaw"] and after["panY"]==0 and after["zoom"]==1
    for name in ("front","back","bottom","left","right","iso"):
        assert call(name)["workspace"]=="3d"
    assert page.evaluate("CNCSolidExchange.model()")==model
    assert call("fit-bed")==bed
    assert page.evaluate("KerfKiteViews.current()")=="2d"
    page.set_viewport_size({"width":390,"height":844})
    assert page.evaluate("document.documentElement.scrollWidth <= window.innerWidth")
    print("PASS 2D fit, cursor-anchored zoom, pan and mouse drag at scale; 3D keyboard views and camera-only changes")
