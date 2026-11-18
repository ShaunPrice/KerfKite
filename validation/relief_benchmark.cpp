// Benchmark-only translation of studio.relief's contact loop. No machine I/O.
// Input is a trusted fixture from stress_cam.py, not a production API.
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>
int main() {
  int rows, cols; double width,height,depth,diameter,step;
  if (!(std::cin>>rows>>cols>>width>>height>>depth>>diameter>>step)) return 2;
  if(rows<2||rows>256||cols<2||cols>256||width<=0||height<=0||step<=0) return 2;
  std::vector<std::vector<double>> v(rows,std::vector<double>(cols));
  for(auto &row:v) for(auto &x:row) {if(!(std::cin>>x)) return 2; x=(x-1)*depth;}
  double dx=width/(cols-1),dy=height/(rows-1),r=diameter/2;
  int nx=std::ceil(width/std::min(dx,step)),ny=std::ceil(height/step);
  if((long long)(nx+1)*(ny+1)>100000) return 2;
  std::vector<std::vector<double>> maxima(rows-1,std::vector<double>(cols-1));
  for(int j=0;j<rows-1;j++) for(int i=0;i<cols-1;i++)
    maxima[j][i]=std::max({v[j][i],v[j][i+1],v[j+1][i],v[j+1][i+1]});
  std::cout<<std::setprecision(17);
  for(int row=0;row<=ny;row++) {
    double y=height*row/ny; std::vector<double> contacts(nx),z(nx+1);
    for(int k=0;k<nx;k++) {
      double a=width*k/nx,b=width*(k+1)/nx,best=-depth;
      int li=std::max(0,(int)std::floor((a-r)/dx)),hi=std::min(cols-2,(int)std::floor((b+r)/dx));
      int lj=std::max(0,(int)std::floor((y-r)/dy)),hj=std::min(rows-2,(int)std::floor((y+r)/dy));
      for(int j=lj;j<=hj;j++) {
        double yd=std::max({j*dy-y,y-(j+1)*dy,0.});
        for(int i=li;i<=hi;i++) {
          double xd=std::max({i*dx-b,a-(i+1)*dx,0.}),ds=xd*xd+yd*yd;
          if(ds>r*r) continue;
          best=std::max(best,maxima[j][i]-(r-std::sqrt(std::max(0.,r*r-ds))));
        }
      }
      contacts[k]=std::min(0.,best);
    }
    z[0]=contacts[0];z[nx]=contacts[nx-1];
    for(int k=1;k<nx;k++) z[k]=std::max(contacts[k-1],contacts[k]);
    if(row%2) std::reverse(z.begin(),z.end());
    for(double d:z) std::cout<<d<<'\n';
  }
}
